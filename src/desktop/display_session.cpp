#include "display_session.hpp"
#include "background_capture_runtime.hpp"
#include "display_output_retarget.hpp"

#include <limits>
#include <stdexcept>
#include <utility>

namespace bafx::desktop
{
struct PendingSecondaryOutputRenegotiation final
{
    bafx::windows::CompositionOutputPreference preference{
        bafx::windows::CompositionOutputPreference::ConservativeSdr};
    std::string reason{};
    std::optional<DisplayTarget> target{};
};

struct DisplaySessionBackgroundCaptureState final
{
    bafx::windows::BackgroundCaptureTransition transition{};
    BackgroundCaptureExecutionResult execution{};
    bafx::windows::BackgroundCaptureRequest request{};
    std::uint64_t controlGeneration{0U};
    std::filesystem::path logPath{};
    std::string pendingSensorFailure{};
    CaptureExclusionHealthPoller exclusionHealthPoller{};
    DisplayCaptureSizeTracker captureSizeTracker{};
    std::optional<DisplayTarget> pendingTarget{};
    std::optional<bafx::windows::CompositionOutputPreference>
        pendingTargetOutputPreference{};
    std::optional<bafx::windows::DisplayColorCapabilities>
        pendingTargetColorCapabilities{};
    std::optional<PendingSecondaryOutputRenegotiation>
        pendingOutputRenegotiation{};
    HWND pendingWakeWindow{nullptr};
    bool outcomePending{false};
    bool sensorWasActiveBeforeTransaction{false};
    bool rendererRecoveryPending{false};
    bool rendererRecoveryAdapterChanged{false};
    bool rendererRecoveryBackgroundWasActive{false};
    bool powerUnavailable{false};
    bool powerRecoveryEligible{false};
    bool powerRecoveryPending{false};
};

namespace
{

constexpr std::uint32_t maximumColorRefreshRetries = 3U;

void requireStartedRequest(
    const bafx::windows::BackgroundCaptureRequestResult result)
{
    if (result != bafx::windows::BackgroundCaptureRequestResult::Started)
    {
        throw std::logic_error(
            "Secondary background capture request did not start");
    }
}

void appendSecondaryBackgroundOutcome(
    DisplaySessionBackgroundCaptureState& state,
    const bafx::windows::CompositionRenderer& renderer)
{
    if (!state.outcomePending)
    {
        return;
    }
    appendBackgroundCaptureOutcome(
        state.logPath,
        state.request,
        state.transition,
        state.execution,
        renderer);
    state.outcomePending = false;
}

}

DisplaySession::DisplaySession(DisplaySessionOptions options)
    : borderlessAccessAuthority_(options.borderlessAccessAuthority),
      target_(std::move(options.target)),
      window_(
          options.instance,
          target_.bounds,
          options.title,
          bafx::windows::OverlayWindowOptions::renderSurface()),
      requestedOutputPreference_(options.outputPreference),
      colorCapabilities_(bafx::windows::queryDisplayColorCapabilities(
          target_.monitor)),
      renderer_(
          window_.handle(),
          window_.size(),
          options.bloomSettings,
          options.backgroundStopObserver,
          requestedAdapter(target_),
          resolveDisplayOutputPreference(
              requestedOutputPreference_,
              colorCapabilities_)),
      simulation_(options.simulationSeed)
{
    if (borderlessAccessAuthority_ == nullptr)
    {
        throw std::invalid_argument(
            "Display session requires the process access authority");
    }
    if (!colorCapabilities_.has_value()
        || !bafx::windows::displayColorStateComplete(*colorCapabilities_))
    {
        // Startup can race a display-mode transition. A partial DisplayConfig
        // snapshot is diagnostic evidence, not a stable output decision.
        colorRefreshRetriesRemaining_ = maximumColorRefreshRetries;
    }
    static_cast<void>(colorMonitor_.start(target_.monitor, options.wakeWindow));
}

DisplaySession::~DisplaySession()
{
    shutdownSecondaryBackgroundCapture();
}

const DisplayTarget& DisplaySession::target() const noexcept
{
    return target_;
}

const DisplayTarget& DisplaySession::reconciliationTarget() const noexcept
{
    if (secondaryBackgroundCapture_ != nullptr)
    {
        const DisplaySessionBackgroundCaptureState& state =
            *secondaryBackgroundCapture_;
        const bool transactionOwnsTarget = state.execution.transactionActive
            || state.transition.transitioning();
        if (transactionOwnsTarget && state.pendingTarget.has_value())
        {
            return *state.pendingTarget;
        }
    }
    return target_;
}

bool DisplaySession::retargetPendingFor(
    const DisplayTarget& target) const noexcept
{
    if (secondaryBackgroundCapture_ == nullptr)
    {
        return false;
    }

    const DisplaySessionBackgroundCaptureState& state =
        *secondaryBackgroundCapture_;
    const bool transactionOwnsTarget = state.execution.transactionActive
        || state.transition.transitioning();
    if (!transactionOwnsTarget || !state.pendingTarget.has_value())
    {
        return false;
    }

    const DisplayTarget& pending = *state.pendingTarget;
    return sameDisplayTarget(pending, target)
        && sameDisplaySourceIdentity(pending, target);
}

bafx::windows::OverlayWindow& DisplaySession::window() noexcept
{
    return window_;
}

const bafx::windows::OverlayWindow& DisplaySession::window() const noexcept
{
    return window_;
}

bafx::windows::CompositionRenderer& DisplaySession::renderer() noexcept
{
    return renderer_;
}

const bafx::windows::CompositionRenderer& DisplaySession::renderer() const noexcept
{
    return renderer_;
}

bafx::fx::SimulationRuntime& DisplaySession::simulation() noexcept
{
    return simulation_;
}

const bafx::fx::SimulationRuntime& DisplaySession::simulation() const noexcept
{
    return simulation_;
}

bafx::fx::SimulationTimeline& DisplaySession::timeline() noexcept
{
    return timeline_;
}

bafx::windows::DisplayColorMonitor& DisplaySession::colorMonitor() noexcept
{
    return colorMonitor_;
}

const std::optional<bafx::windows::DisplayColorCapabilities>&
DisplaySession::colorCapabilities() const noexcept
{
    return colorCapabilities_;
}

bafx::windows::CompositionOutputPreference
DisplaySession::requestedOutputPreference() const noexcept
{
    return requestedOutputPreference_;
}

void DisplaySession::setRequestedOutputPreference(
    const bafx::windows::CompositionOutputPreference preference) noexcept
{
    requestedOutputPreference_ = preference;
}

const bafx::windows::DisplayColorMonitorResult&
DisplaySession::colorMonitorResult() const noexcept
{
    // The monitor can recover its WinRT subscription after construction.
    // Return its live result so support diagnostics do not retain a stale
    // startup failure after that retry succeeds.
    return colorMonitor_.result();
}

bool DisplaySession::renderFaulted() const noexcept
{
    return renderFaulted_;
}

bool DisplaySession::lastPresentedDrawableContent() const noexcept
{
    return lastPresentedDrawableContent_;
}

bool DisplaySession::resourceDomainReadyForTarget(
    const DisplayTarget& target) const noexcept
{
    const bafx::windows::GraphicsDeviceInfo& device = renderer_.deviceInfo();
    if (displayTargetResourceAdapterMatches(
            target,
            device.adapterLuid,
            device.driverType == bafx::windows::GraphicsDriverType::Hardware))
    {
        return true;
    }
    if (device.driverType != bafx::windows::GraphicsDriverType::Warp
        || !target.sourceAdapterResolved
        || !device.requestedAdapterLuid.has_value()
        || device.requestedAdapterLuid->HighPart
            != target.sourceAdapterLuid.HighPart
        || device.requestedAdapterLuid->LowPart
            != target.sourceAdapterLuid.LowPart)
    {
        return false;
    }

    // A found adapter that rejected D3D creation is terminal for this resource
    // domain. A previously absent adapter is retried only after DXGI can see it,
    // so DRR or DPI metadata churn cannot recreate WARP once per poll.
    return device.requestedAdapterFound
        || !renderer_.requestedAdapterPresent();
}

bool DisplaySession::framePacingDue(
    const bafx::core::MonotonicTime now) const noexcept
{
    return !nextFramePacingDeadline_.has_value()
        || now >= *nextFramePacingDeadline_;
}

std::optional<bafx::core::MonotonicTime>
DisplaySession::nextFramePacingDeadline() const noexcept
{
    return nextFramePacingDeadline_;
}

void DisplaySession::acceptAppliedTarget(
    DisplayTarget target,
    const HWND wakeWindow) noexcept
{
    const bool sourceChanged = target_.monitor != target.monitor
        || !sameDisplaySourceIdentity(target_, target);
    target_ = std::move(target);
    if (sourceChanged)
    {
        // Capabilities describe one physical output. Never use a coherent
        // snapshot from the previous source as evidence for the new target.
        colorCapabilities_.reset();
        colorRefreshRetriesRemaining_ = maximumColorRefreshRetries;
    }
    if (secondaryBackgroundCapture_ != nullptr)
    {
        // A topology transaction creates a new output contract. A size sample
        // from the preceding target cannot decide the new WGC session's fate.
        secondaryBackgroundCapture_->captureSizeTracker.reset();
    }
    static_cast<void>(colorMonitor_.start(target_.monitor, wakeWindow));
}

bafx::windows::BackgroundCadenceRefreshResult
DisplaySession::updateTargetMetadata(DisplayTarget target) noexcept
{
    target_ = std::move(target);
    // DPI and refresh-rate changes preserve the physical resource domain.
    // Refresh only the sample-age policy so a mixed-refresh desktop does not
    // inherit the previous cadence or pay for a WGC session restart.
    // Consume the same stabilized snapshot that updated target_. A second
    // DisplayConfig query could transiently fail and leave producer cadence
    // inconsistent with the metadata that triggered this update.
    return renderer_.refreshBackgroundCadence(
        target_.monitor,
        target_.captureRefreshRate);
}

DisplaySessionRetargetResult DisplaySession::retargetFxOnly(
    DisplayTarget target,
    const HWND wakeWindow)
{
    try
    {
        const std::optional<bafx::windows::DisplayColorCapabilities>
            targetColorCapabilities =
                bafx::windows::queryDisplayColorCapabilities(target.monitor);
        const bafx::windows::CompositionOutputPreference targetPreference =
            resolveDisplayOutputPreference(
                requestedOutputPreference_,
                targetColorCapabilities);
        const DisplayOutputRetargetResult output = retargetDisplayOutput(
            window_,
            renderer_,
            DisplayOutputRetargetIntent{
                target.bounds,
                requestedAdapter(target),
                displayTargetSize(target),
                targetPreference});
        lastPresentedDrawableContent_ = false;
        resetFramePacing();
        acceptAppliedTarget(std::move(target), wakeWindow);
        colorCapabilities_ = targetColorCapabilities;
        colorRefreshRetriesRemaining_ = targetColorCapabilities.has_value()
                && bafx::windows::displayColorStateComplete(
                    *targetColorCapabilities)
            ? 0U
            : maximumColorRefreshRetries;
        clearRenderFault();
        return DisplaySessionRetargetResult{
            output.adapter,
            output.output};
    }
    catch (const DisplayOutputRollbackError&)
    {
        // A failed rollback leaves this independent surface untrustworthy;
        // keep it out of frame pacing until topology reconciliation replaces it.
        markRenderFaulted();
        throw;
    }
}

DisplaySessionRetargetResult DisplaySession::retargetSecondary(
    DisplayTarget target,
    const HWND wakeWindow)
{
    if (secondaryBackgroundCapture_ == nullptr)
    {
        return retargetFxOnly(std::move(target), wakeWindow);
    }

    DisplaySessionBackgroundCaptureState& state =
        *secondaryBackgroundCapture_;
    if (state.execution.transactionActive)
    {
        const BackgroundCaptureExecutionStatus canceled =
            cancelBackgroundCaptureTransition(
                state.transition,
                window_,
                renderer_,
                *borderlessAccessAuthority_,
                state.execution,
                BackgroundCaptureCancelResizePolicy::Discard,
                "secondary-display-target",
                state.logPath);
        if (canceled != BackgroundCaptureExecutionStatus::Completed)
        {
            throw std::logic_error(
                "Secondary display retarget cancellation remained pending");
        }
        acceptPendingSecondaryTargetIfApplied(state);
        appendSecondaryBackgroundOutcome(state, renderer_);
    }

    const std::optional<bafx::windows::DisplayColorCapabilities>
        targetColorCapabilities =
            bafx::windows::queryDisplayColorCapabilities(target.monitor);
    state.pendingTargetOutputPreference = resolveDisplayOutputPreference(
        requestedOutputPreference_,
        targetColorCapabilities);
    state.pendingTargetColorCapabilities = targetColorCapabilities;
    state.pendingTarget = std::move(target);
    state.captureSizeTracker.reset();
    state.pendingWakeWindow = wakeWindow;
    state.sensorWasActiveBeforeTransaction =
        renderer_.backgroundCaptureActive();
    const bafx::windows::BackgroundCaptureRequestResult requestResult =
        state.transition.beginIntent(
            state.request,
            displayTargetSize(*state.pendingTarget));
    if (requestResult ==
        bafx::windows::BackgroundCaptureRequestResult::NoChange)
    {
        acceptAppliedTarget(
            std::move(*state.pendingTarget),
            state.pendingWakeWindow);
        static_cast<void>(refreshColorCapabilities(
            state.pendingTargetColorCapabilities));
        state.pendingTarget.reset();
        state.pendingTargetOutputPreference.reset();
        state.pendingTargetColorCapabilities.reset();
        state.pendingWakeWindow = nullptr;
        return {};
    }
    requireStartedRequest(requestResult);
    state.outcomePending = true;

    static_cast<void>(serviceSecondaryBackgroundCapture(
        bafx::core::MonotonicTime::zero()));
    return DisplaySessionRetargetResult{
        state.execution.outputAdapterRetargeted
            ? (state.execution.outputAdapterWarpFallback
                ? bafx::windows::OutputAdapterRetargetStatus::
                    RecreatedWarpFallback
                : bafx::windows::OutputAdapterRetargetStatus::
                    RecreatedHardware)
            : bafx::windows::OutputAdapterRetargetStatus::Unchanged,
        state.execution.resizedOutputSize.has_value()
            ? (state.execution.deviceRecovered
                ? bafx::windows::OutputResizeStatus::DeviceRecovered
                : bafx::windows::OutputResizeStatus::Resized)
            : bafx::windows::OutputResizeStatus::Unchanged,
        state.execution.transactionActive || state.transition.transitioning()};
}

DisplaySessionDeviceRecoveryResult DisplaySession::tryRecoverDevice() noexcept
{
    const bafx::windows::GraphicsDeviceInfo previousDeviceInfo =
        renderer_.deviceInfo();
    const bool backgroundWasActive = renderer_.backgroundCaptureActive();
    if (!renderer_.tryRecoverDevice())
    {
        return DisplaySessionDeviceRecoveryResult{
            false,
            false,
            backgroundWasActive,
            DisplaySessionBackgroundRecoveryStatus::NotRequired};
    }
    return finishDeviceRecovery(previousDeviceInfo, backgroundWasActive);
}

DisplaySessionDeviceRecoveryResult DisplaySession::setBloomSettings(
    const bafx::windows::FxBloomSettings settings)
{
    const bafx::windows::GraphicsDeviceInfo previousDeviceInfo =
        renderer_.deviceInfo();
    const bool backgroundWasActive = renderer_.backgroundCaptureActive();
    if (!renderer_.setBloomSettings(settings))
    {
        return {};
    }
    return finishDeviceRecovery(previousDeviceInfo, backgroundWasActive);
}

void DisplaySession::initializeSecondaryBackgroundCapture(
    const bafx::windows::BackgroundCaptureRequest request,
    const std::uint64_t controlGeneration,
    const std::filesystem::path& logPath,
    const bool powerUnavailable)
{
    if (secondaryBackgroundCapture_ != nullptr)
    {
        throw std::logic_error(
            "Secondary background capture is already initialized");
    }

    auto state = std::make_unique<DisplaySessionBackgroundCaptureState>();
    state->request = request;
    state->controlGeneration = controlGeneration;
    state->logPath = logPath;
    state->powerUnavailable = powerUnavailable;
    // An explicit request created while scan-out is unavailable still belongs
    // to this owner. Park it now and consume it once the display is restored.
    state->powerRecoveryEligible = powerUnavailable && request.sensorRequired;
    const bafx::windows::BackgroundCaptureRequestResult requestResult =
        powerUnavailable && request.sensorRequired
        ? state->transition.beginPowerSuspension(request)
        : state->transition.beginRequest(request);
    requireStartedRequest(requestResult);
    state->outcomePending = true;
    secondaryBackgroundCapture_ = std::move(state);
    static_cast<void>(serviceSecondaryBackgroundCapture(
        bafx::core::MonotonicTime::zero()));
}

void DisplaySession::updateSecondaryBackgroundCaptureRequest(
    bafx::windows::BackgroundCaptureRequest request,
    const std::uint64_t controlGeneration)
{
    if (secondaryBackgroundCapture_ == nullptr)
    {
        throw std::logic_error(
            "Secondary background capture is not initialized");
    }

    DisplaySessionBackgroundCaptureState& state =
        *secondaryBackgroundCapture_;
    // Retry identity belongs to this display's recovery state machine. The
    // process-wide control snapshot only supplies the requested path/profile.
    request.retryToken = state.request.retryToken;
    if (state.request == request)
    {
        if (!state.execution.transactionActive)
        {
            state.controlGeneration = controlGeneration;
        }
        return;
    }

    if (state.execution.transactionActive)
    {
        const BackgroundCaptureExecutionStatus canceled =
            cancelBackgroundCaptureTransition(
                state.transition,
                window_,
                renderer_,
                *borderlessAccessAuthority_,
                state.execution,
                BackgroundCaptureCancelResizePolicy::Preserve,
                "secondary-control-generation",
                state.logPath);
        if (canceled != BackgroundCaptureExecutionStatus::Completed)
        {
            throw std::logic_error(
                "Secondary background capture cancellation remained pending");
        }
        // A preserved resize may have moved the surface before the new
        // configuration generation starts. Commit that geometry now so
        // pointer routing and monitor facts cannot remain on the old target.
        acceptPendingSecondaryTargetIfApplied(state);
        appendSecondaryBackgroundOutcome(state, renderer_);
    }

    state.request = request;
    state.controlGeneration = controlGeneration;
    state.captureSizeTracker.reset();
    if (!state.request.sensorRequired)
    {
        state.powerRecoveryEligible = false;
        state.powerRecoveryPending = false;
    }
    else if (state.powerUnavailable)
    {
        // A control-plane change is a fresh explicit request. It cannot start
        // WGC while scan-out is unavailable, so retain it for the restore edge.
        state.powerRecoveryEligible = true;
    }
    state.sensorWasActiveBeforeTransaction =
        renderer_.backgroundCaptureActive();
    const bafx::windows::BackgroundCaptureRequestResult requestResult =
        state.powerUnavailable && request.sensorRequired
        ? state.transition.beginPowerSuspension(request)
        : state.transition.beginRequest(request);
    requireStartedRequest(requestResult);
    state.outcomePending = true;
    static_cast<void>(serviceSecondaryBackgroundCapture(
        bafx::core::MonotonicTime::zero()));
}

void DisplaySession::requestSecondaryOutputRenegotiation(
    const bafx::windows::CompositionOutputPreference preference,
    const std::string_view reason,
    std::optional<DisplayTarget> target)
{
    if (secondaryBackgroundCapture_ == nullptr)
    {
        throw std::logic_error(
            "Secondary background capture is not initialized");
    }

    const std::optional<PendingSecondaryOutputRenegotiation>& pending =
        secondaryBackgroundCapture_->pendingOutputRenegotiation;
    if (target.has_value()
        && pending.has_value()
        && !pending->target.has_value())
    {
        // A configuration request applies to whichever target commits next.
        // A monitor event from the old target must not replace that policy.
        return;
    }

    // Collapse repeated target notifications to the newest contract. The
    // service owner performs the mutation after any earlier WGC transaction.
    secondaryBackgroundCapture_->pendingOutputRenegotiation =
        PendingSecondaryOutputRenegotiation{
            preference,
            std::string(reason),
            std::move(target)};
}

DisplaySessionBackgroundCaptureServiceResult
DisplaySession::serviceSecondaryBackgroundCapture(
    const bafx::core::MonotonicTime now)
{
    DisplaySessionBackgroundCaptureServiceResult result{};
    if (secondaryBackgroundCapture_ == nullptr)
    {
        return result;
    }

    DisplaySessionBackgroundCaptureState& state =
        *secondaryBackgroundCapture_;
    constexpr std::size_t maximumServiceTransitions = 4U;
    for (std::size_t attempt = 0U;
         attempt < maximumServiceTransitions;
         ++attempt)
    {
        if (state.transition.transitioning())
        {
            const DisplayTargetIntent intent = state.execution.transactionActive
                ? state.execution.targetIntent
                : (state.pendingTarget.has_value()
                    ? DisplayTargetIntent{
                        *state.pendingTarget,
                        true,
                        state.pendingTargetOutputPreference,
                        state.pendingTargetColorCapabilities}
                    : DisplayTargetIntent{target_, false});
            const std::uint64_t generation = state.execution.transactionActive
                ? state.execution.controlGeneration
                : state.controlGeneration;
            const BackgroundCaptureExecutionStatus status =
                executeBackgroundCaptureTransition(
                    state.transition,
                    window_,
                    renderer_,
                    intent,
                    generation,
                    *borderlessAccessAuthority_,
                    state.execution,
                    state.logPath);
            if (status == BackgroundCaptureExecutionStatus::Pending)
            {
                result.active = renderer_.backgroundCaptureActive();
                return result;
            }

            result.renderInvalidated = true;
            acceptPendingSecondaryTargetIfApplied(state);
            if (!state.pendingSensorFailure.empty()
                && state.execution.sensorFailure.empty())
            {
                state.execution.sensorFailure = state.pendingSensorFailure;
            }
            state.pendingSensorFailure.clear();
            if (state.rendererRecoveryPending)
            {
                // Recovery can occur after an intent is queued but before its
                // first action starts. Merge that fact only after execution
                // has created its result object so it cannot be reset.
                state.execution.deviceRecovered = true;
                state.execution.deviceRecoveryAdapterChanged =
                    state.execution.deviceRecoveryAdapterChanged
                    || state.rendererRecoveryAdapterChanged;
                state.sensorWasActiveBeforeTransaction =
                    state.sensorWasActiveBeforeTransaction
                    || state.rendererRecoveryBackgroundWasActive;
                state.rendererRecoveryPending = false;
                state.rendererRecoveryAdapterChanged = false;
                state.rendererRecoveryBackgroundWasActive = false;
            }
            result.deviceRecovered = result.deviceRecovered
                || state.execution.deviceRecovered;
            if (renderer_.backgroundCaptureActive())
            {
                // Permission can remain pending while DRR metadata advances.
                // Start from the transaction snapshot, then converge once to
                // the latest committed metadata without recreating the Sensor.
                static_cast<void>(renderer_.refreshBackgroundCadence(
                    target_.monitor,
                    target_.captureRefreshRate));
            }
            appendSecondaryBackgroundOutcome(state, renderer_);
            if (state.execution.deviceRecovered)
            {
                // Resize/output actions continue through StartSensor after a
                // successful recovery. Do not replace that healthy session
                // merely because the transaction also recorded a recovery.
                const bool retryEligible =
                    !renderer_.backgroundCaptureActive()
                    && canRetryBackgroundCaptureAfterDeviceRecovery(
                        state.request.sensorRequired,
                        state.sensorWasActiveBeforeTransaction,
                        state.execution.deviceRecoveryAdapterChanged,
                        renderer_.deviceInfo().driverType,
                        renderer_.backgroundCaptureRestartAllowed());
                if (retryEligible)
                {
                    if (state.request.retryToken
                        == std::numeric_limits<std::uint64_t>::max())
                    {
                        throw std::runtime_error(
                            "Secondary WGC retry token exhausted");
                    }
                    ++state.request.retryToken;
                    state.sensorWasActiveBeforeTransaction = false;
                    requireStartedRequest(
                        state.transition.beginRequest(state.request));
                    state.outcomePending = true;
                    continue;
                }
            }
        }

        if (state.powerUnavailable)
        {
            // Pending output/color work remains queued until the display has a
            // scan-out contract again. In particular, do not interpret the
            // intentionally stopped producer as an unexpected session loss.
            result.active = renderer_.backgroundCaptureActive();
            return result;
        }

        if (state.pendingOutputRenegotiation.has_value())
        {
            const PendingSecondaryOutputRenegotiation pending =
                *state.pendingOutputRenegotiation;
            state.pendingOutputRenegotiation.reset();
            state.captureSizeTracker.reset();
            result.outputRenegotiationPreference = pending.preference;
            result.outputRenegotiationReason = pending.reason;
            const bool staleTarget = pending.target.has_value()
                && (!sameDisplayTarget(*pending.target, target_)
                    || !sameDisplaySourceIdentity(*pending.target, target_));
            if (staleTarget)
            {
                // The retarget transaction already rebuilt the output for the
                // new monitor. Never stop its fresh WGC session for an event
                // that was sampled from the previous target.
                result.outputRenegotiationDiscarded = true;
                result.outputRenegotiationTarget = pending.target;
                result.active = renderer_.backgroundCaptureActive();
                return result;
            }

            const bool backgroundCaptureWasEffective =
                state.transition.effectivePath()
                == bafx::windows::EffectiveBackgroundCapturePath::
                    BackgroundAware;
            const bool sensorWasActive = renderer_.backgroundCaptureActive();
            const bool restartRequired = state.request.sensorRequired
                && backgroundCaptureWasEffective
                && sensorWasActive;
            if (restartRequired
                && state.request.retryToken
                    == std::numeric_limits<std::uint64_t>::max())
            {
                throw std::runtime_error(
                    "Secondary WGC retry token exhausted before output renegotiation");
            }

            // Output resources and WGC textures share one device domain. Stop
            // first even when the current request is FX-only so stale callback
            // diagnostics are consumed before the swap chain is replaced.
            renderer_.disableBackgroundCapture();
            const bafx::windows::WgcBackgroundStopDiagnostics stopDiagnostics =
                appendBackgroundCaptureStopDiagnostics(
                    state.logPath,
                    renderer_,
                    "secondary-output-renegotiation");
            const bafx::windows::GraphicsDeviceInfo previousDeviceInfo =
                renderer_.deviceInfo();
            bool recoveredDuringAttempt = false;
            if (!stopDiagnostics.overallSucceeded)
            {
                result.outputRenegotiationFailure =
                    "WGC stop failed before secondary output renegotiation";
                state.pendingSensorFailure =
                    result.outputRenegotiationFailure;
            }
            else
            {
                const bool recoveryBudgetWasConsumed =
                    renderer_.deviceRecoveryBudgetConsumed();
                try
                {
                    result.outputRenegotiation =
                        renderer_.renegotiateOutput(pending.preference);
                    lastPresentedDrawableContent_ = false;
                    resetFramePacing();
                    recoveredDuringAttempt =
                        result.outputRenegotiation->deviceRecovered;
                }
                catch (const std::exception& error)
                {
                    recoveredDuringAttempt = !recoveryBudgetWasConsumed
                        && renderer_.deviceRecoveryBudgetConsumed();
                    if (recoveredDuringAttempt
                        && !renderer_.deviceRecoveryFailure().empty())
                    {
                        throw std::runtime_error(
                            "Secondary output device recovery failed: "
                            + std::string(renderer_.deviceRecoveryFailure()));
                    }
                    result.outputRenegotiationFailure = error.what();
                }
                catch (...)
                {
                    recoveredDuringAttempt = !recoveryBudgetWasConsumed
                        && renderer_.deviceRecoveryBudgetConsumed();
                    if (recoveredDuringAttempt
                        && !renderer_.deviceRecoveryFailure().empty())
                    {
                        throw std::runtime_error(
                            "Secondary output device recovery failed: "
                            + std::string(renderer_.deviceRecoveryFailure()));
                    }
                    result.outputRenegotiationFailure =
                        "unknown secondary output renegotiation failure";
                }
            }

            if (recoveredDuringAttempt)
            {
                lastPresentedDrawableContent_ = false;
                resetFramePacing();
            }
            result.deviceRecovered = result.deviceRecovered
                || recoveredDuringAttempt;
            result.renderInvalidated = true;
            const bool adapterChanged =
                previousDeviceInfo.adapterLuid.LowPart
                    != renderer_.deviceInfo().adapterLuid.LowPart
                || previousDeviceInfo.adapterLuid.HighPart
                    != renderer_.deviceInfo().adapterLuid.HighPart;
            const bool restartAllowed = restartRequired
                && stopDiagnostics.overallSucceeded
                && (!recoveredDuringAttempt
                    || canRetryBackgroundCaptureAfterDeviceRecovery(
                        true,
                        sensorWasActive,
                        adapterChanged,
                        renderer_.deviceInfo().driverType,
                        renderer_.backgroundCaptureRestartAllowed()));
            if (restartAllowed)
            {
                ++state.request.retryToken;
                // This restart already rebinds the post-resume output, so a
                // queued power recovery must not immediately restart it again.
                state.powerRecoveryPending = false;
                state.sensorWasActiveBeforeTransaction = false;
                requireStartedRequest(
                    state.transition.beginRequest(state.request));
                state.outcomePending = true;
                continue;
            }
        }

        if (state.powerRecoveryPending)
        {
            const bool retryAllowed = state.request.sensorRequired
                && renderer_.deviceInfo().driverType
                    == bafx::windows::GraphicsDriverType::Hardware
                && renderer_.backgroundCaptureRestartAllowed();
            state.powerRecoveryPending = false;
            if (retryAllowed)
            {
                if (state.request.retryToken
                    == (std::numeric_limits<std::uint64_t>::max)())
                {
                    throw std::runtime_error(
                        "Secondary WGC retry token exhausted after display power recovery");
                }

                ++state.request.retryToken;
                state.sensorWasActiveBeforeTransaction =
                    renderer_.backgroundCaptureActive();
                requireStartedRequest(
                    state.transition.beginRequest(state.request));
                state.outcomePending = true;
                continue;
            }
        }

        const bool active = renderer_.backgroundCaptureActive();
        const bool expectedActive = state.transition.effectivePath()
            == bafx::windows::EffectiveBackgroundCapturePath::BackgroundAware;
        if (expectedActive && !active)
        {
            const std::string stoppedReason(
                renderer_.backgroundCaptureFailure());
            if (!state.transition.beginSessionStopped())
            {
                throw std::logic_error(
                    "Secondary stopped WGC session could not enter cleanup");
            }
            state.sensorWasActiveBeforeTransaction = false;
            state.outcomePending = true;
            if (state.pendingSensorFailure.empty())
            {
                state.pendingSensorFailure = stoppedReason;
            }
            continue;
        }

        if (active)
        {
            const std::optional<DisplayCaptureSizeMismatch> mismatch =
                state.captureSizeTracker.takeConfirmedMismatch();
            if (mismatch.has_value())
            {
                state.pendingSensorFailure =
                    formatDisplayCaptureSizeMismatch(*mismatch);
                if (!state.transition.beginCaptureSizeMismatch())
                {
                    throw std::logic_error(
                        "Secondary confirmed WGC size mismatch could not enter cleanup");
                }
                state.sensorWasActiveBeforeTransaction = true;
                state.outcomePending = true;
                continue;
            }
        }

        if (active)
        {
            if (const std::optional<bafx::windows::WindowSize> captureSize =
                    renderer_.pendingBackgroundFramePoolSize();
                captureSize.has_value())
            {
                const bafx::windows::WindowSize outputSize =
                    renderer_.outputSize();
                // WGC can publish rotation or mode dimensions before the shell
                // posts a topology message. Recreate the producer, but require
                // topology confirmation before treating a mismatch as fatal.
                static_cast<void>(state.captureSizeTracker.observeCaptureSize(
                    *captureSize,
                    outputSize));
                if (!state.transition.beginFramePoolRecreate(*captureSize))
                {
                    throw std::logic_error(
                        "Secondary WGC frame pool resize was rejected");
                }
                state.sensorWasActiveBeforeTransaction = true;
                state.outcomePending = true;
                continue;
            }

            if (state.exclusionHealthPoller.shouldQuery(true, now))
            {
                const bafx::windows::CaptureExclusionQueryStatus affinity =
                    window_.queryCaptureExcluded(true);
                if (!affinity.confirmed())
                {
                    appendCaptureExclusionHealthFailure(
                        state.logPath,
                        state.controlGeneration,
                        false,
                        affinity);
                    if (!state.transition.beginCaptureExclusionLost())
                    {
                        throw std::logic_error(
                            "Secondary capture exclusion loss was rejected");
                    }
                    state.sensorWasActiveBeforeTransaction = true;
                    state.outcomePending = true;
                    continue;
                }
            }
        }

        result.active = active;
        return result;
    }

    throw std::logic_error(
        "Secondary background capture service exceeded its transition budget");
}

DisplaySessionBackgroundCaptureServiceResult
DisplaySession::handleSecondaryBorderlessAccessLost(
    const bafx::core::MonotonicTime now)
{
    DisplaySessionBackgroundCaptureServiceResult result{};
    if (secondaryBackgroundCapture_ == nullptr)
    {
        return result;
    }

    DisplaySessionBackgroundCaptureState& state =
        *secondaryBackgroundCapture_;
    if (!state.request.sensorRequired || state.request.allowSystemBorder)
    {
        result.active = renderer_.backgroundCaptureActive();
        return result;
    }

    if (state.execution.transactionActive)
    {
        const BackgroundCaptureExecutionStatus canceled =
            cancelBackgroundCaptureTransition(
                state.transition,
                window_,
                renderer_,
                *borderlessAccessAuthority_,
                state.execution,
                BackgroundCaptureCancelResizePolicy::Preserve,
                "secondary-borderless-access-lost",
                state.logPath);
        if (canceled != BackgroundCaptureExecutionStatus::Completed)
        {
            throw std::logic_error(
                "Secondary borderless access cancellation remained pending");
        }
        acceptPendingSecondaryTargetIfApplied(state);
        appendSecondaryBackgroundOutcome(state, renderer_);
        result.renderInvalidated = true;
    }
    else if (state.transition.transitioning())
    {
        throw std::logic_error(
            "Secondary borderless access loss found an unowned transition");
    }

    if (state.transition.effectivePath()
            != bafx::windows::EffectiveBackgroundCapturePath::BackgroundAware
        || !renderer_.backgroundCaptureActive())
    {
        result.active = renderer_.backgroundCaptureActive();
        return result;
    }

    state.sensorWasActiveBeforeTransaction = true;
    state.pendingSensorFailure =
        "Borderless capture access was revoked while WGC was active";
    if (!state.transition.beginBorderlessAccessLost())
    {
        throw std::logic_error(
            "Secondary borderless access loss could not enter cleanup");
    }
    state.outcomePending = true;
    DisplaySessionBackgroundCaptureServiceResult cleanup =
        serviceSecondaryBackgroundCapture(now);
    cleanup.renderInvalidated =
        cleanup.renderInvalidated || result.renderInvalidated;
    return cleanup;
}

bool DisplaySession::retrySecondaryBorderlessAccess(
    const std::uint64_t controlGeneration)
{
    if (secondaryBackgroundCapture_ == nullptr)
    {
        return false;
    }

    DisplaySessionBackgroundCaptureState& state =
        *secondaryBackgroundCapture_;
    if (state.powerUnavailable
        || !state.request.sensorRequired
        || state.request.allowSystemBorder
        || state.execution.transactionActive
        || state.transition.transitioning()
        || renderer_.deviceInfo().driverType
            != bafx::windows::GraphicsDriverType::Hardware
        || !renderer_.backgroundCaptureRestartAllowed())
    {
        return false;
    }
    if (state.transition.effectivePath()
            == bafx::windows::EffectiveBackgroundCapturePath::BackgroundAware
        && renderer_.backgroundCaptureActive())
    {
        return false;
    }
    if (state.request.retryToken
        == (std::numeric_limits<std::uint64_t>::max)())
    {
        throw std::runtime_error(
            "Secondary borderless access retry token exhausted");
    }

    // AccessChanged is the explicit recovery boundary. Advance this display's
    // local token once so ordinary render iterations cannot reopen WGC.
    ++state.request.retryToken;
    state.controlGeneration = controlGeneration;
    state.sensorWasActiveBeforeTransaction = false;
    requireStartedRequest(state.transition.beginRequest(state.request));
    state.outcomePending = true;
    return true;
}

bool DisplaySession::requestSecondaryPowerRecovery(
    const std::uint64_t controlGeneration) noexcept
{
    if (secondaryBackgroundCapture_ == nullptr)
    {
        return false;
    }

    DisplaySessionBackgroundCaptureState& state =
        *secondaryBackgroundCapture_;
    const bool eligible = state.powerRecoveryEligible;
    // A restore edge consumes the preceding unavailable edge exactly once,
    // even when policy or hardware now blocks the actual restart.
    state.powerUnavailable = false;
    state.powerRecoveryEligible = false;
    if (!eligible
        || !state.request.sensorRequired
        || state.powerRecoveryPending)
    {
        return false;
    }

    // The display-power edge is the external state change that authorizes one
    // retry. The service owner consumes it after any older output transaction.
    state.controlGeneration = controlGeneration;
    state.powerRecoveryPending = true;
    return true;
}

DisplaySessionBackgroundCaptureServiceResult
DisplaySession::suspendSecondaryBackgroundCaptureForPower(
    const bafx::core::MonotonicTime now)
{
    DisplaySessionBackgroundCaptureServiceResult result{};
    if (secondaryBackgroundCapture_ == nullptr)
    {
        return result;
    }

    DisplaySessionBackgroundCaptureState& state =
        *secondaryBackgroundCapture_;
    state.powerUnavailable = true;
    state.captureSizeTracker.reset();
    state.powerRecoveryPending = false;
    // WGC may report its stop before the power message reaches this owner. The
    // committed effective path is the durable evidence; an older terminal
    // FX-only failure remains ineligible for an implicit restart.
    state.powerRecoveryEligible = state.powerRecoveryEligible
        || (state.request.sensorRequired
            && state.transition.effectivePath()
                == bafx::windows::EffectiveBackgroundCapturePath::BackgroundAware);
    if (state.execution.transactionActive)
    {
        const BackgroundCaptureExecutionStatus canceled =
            cancelBackgroundCaptureTransition(
                state.transition,
                window_,
                renderer_,
                *borderlessAccessAuthority_,
                state.execution,
                BackgroundCaptureCancelResizePolicy::Preserve,
                "secondary-display-power-unavailable",
                state.logPath);
        if (canceled != BackgroundCaptureExecutionStatus::Completed)
        {
            throw std::logic_error(
                "Secondary display-power cancellation remained pending");
        }
        acceptPendingSecondaryTargetIfApplied(state);
        appendSecondaryBackgroundOutcome(state, renderer_);
        result.renderInvalidated = true;
    }
    else if (state.transition.transitioning())
    {
        throw std::logic_error(
            "Secondary display-power suspension found an unowned transition");
    }

    if (!state.request.sensorRequired)
    {
        result.active = renderer_.backgroundCaptureActive();
        return result;
    }

    const bafx::windows::BackgroundCaptureRequestResult requestResult =
        state.transition.beginPowerSuspension(state.request);
    if (requestResult
        == bafx::windows::BackgroundCaptureRequestResult::NoChange)
    {
        result.active = renderer_.backgroundCaptureActive();
        return result;
    }
    requireStartedRequest(requestResult);
    state.sensorWasActiveBeforeTransaction = false;
    state.outcomePending = true;
    DisplaySessionBackgroundCaptureServiceResult suspended =
        serviceSecondaryBackgroundCapture(now);
    suspended.renderInvalidated =
        suspended.renderInvalidated || result.renderInvalidated;
    return suspended;
}

bool DisplaySession::secondaryBackgroundCaptureInitialized() const noexcept
{
    return secondaryBackgroundCapture_ != nullptr;
}

bool DisplaySession::secondaryBackgroundCaptureActive() const noexcept
{
    return secondaryBackgroundCapture_ != nullptr
        && renderer_.backgroundCaptureActive();
}

HANDLE DisplaySession::secondaryBackgroundFrameAvailableObject() const noexcept
{
    return secondaryBackgroundCapture_ != nullptr
        ? renderer_.backgroundFrameAvailableObject()
        : nullptr;
}

bool DisplaySession::takeTopologyRefreshRequest() noexcept
{
    return secondaryBackgroundCapture_ != nullptr
        && secondaryBackgroundCapture_->captureSizeTracker
            .takeTopologyRefreshRequest();
}

void DisplaySession::observeSecondaryCaptureTopology(
    const DisplayTargetSnapshot& topology) noexcept
{
    if (secondaryBackgroundCapture_ == nullptr
        || topology.status != bafx::windows::DisplayTopologyStatus::Complete)
    {
        return;
    }

    const DisplayTarget& expected = reconciliationTarget();
    const DisplayTarget* observed = findDisplayTargetBySource(
        topology,
        expected);
    if (observed == nullptr)
    {
        observed = findDisplayTargetByLogicalSlot(topology, expected);
    }
    if (observed == nullptr)
    {
        // Reconciliation owns authoritative removal. Do not diagnose a size
        // mismatch for a display absent from the complete snapshot.
        return;
    }

    secondaryBackgroundCapture_->captureSizeTracker.confirmOutputSize(
        displayTargetSize(*observed));
}

void DisplaySession::shutdownSecondaryBackgroundCapture() noexcept
{
    if (secondaryBackgroundCapture_ == nullptr)
    {
        return;
    }

    try
    {
        renderer_.disableBackgroundCapture();
        static_cast<void>(appendBackgroundCaptureStopDiagnostics(
            secondaryBackgroundCapture_->logPath,
            renderer_,
            "secondary-shutdown"));
        static_cast<void>(window_.setCaptureExcluded(false));
    }
    catch (...)
    {
        // Renderer and HWND destructors still release their owned resources.
    }
    secondaryBackgroundCapture_.reset();
}

bool DisplaySession::colorRefreshRetryPending() const noexcept
{
    return colorRefreshRetriesRemaining_ > 0U;
}

std::uint32_t DisplaySession::colorRefreshRetriesRemaining() const noexcept
{
    return colorRefreshRetriesRemaining_;
}

DisplaySessionColorRefreshStatus DisplaySession::refreshColorCapabilities(
    const std::optional<bafx::windows::DisplayColorCapabilities>& fallback,
    const DisplaySessionColorRefreshRequest request)
    noexcept
{
    if (request == DisplaySessionColorRefreshRequest::Retry
        && colorRefreshRetriesRemaining_ == 0U)
    {
        return colorCapabilities_.has_value()
            ? DisplaySessionColorRefreshStatus::RetainedLastKnownSnapshot
            : DisplaySessionColorRefreshStatus::Unavailable;
    }

    std::optional<bafx::windows::DisplayColorCapabilities> refreshed =
        bafx::windows::queryDisplayColorCapabilities(target_.monitor);
    if (refreshed.has_value()
        && bafx::windows::displayColorStateComplete(*refreshed))
    {
        colorCapabilities_ = std::move(refreshed);
        colorRefreshRetriesRemaining_ = 0U;
        return DisplaySessionColorRefreshStatus::Refreshed;
    }

    if (request == DisplaySessionColorRefreshRequest::Retry)
    {
        --colorRefreshRetriesRemaining_;
    }
    else
    {
        // A new OS notification opens one bounded retry window. This also
        // covers partial DisplayConfig snapshots, which must not replace the
        // last complete HDR contract during a mode transition.
        colorRefreshRetriesRemaining_ = maximumColorRefreshRetries;
    }
    if (fallback.has_value())
    {
        colorCapabilities_ = fallback;
        return DisplaySessionColorRefreshStatus::RetainedTransactionSnapshot;
    }
    if (colorCapabilities_.has_value())
    {
        return DisplaySessionColorRefreshStatus::RetainedLastKnownSnapshot;
    }
    return DisplaySessionColorRefreshStatus::Unavailable;
}

void DisplaySession::recordPresentedFrame(
    const bool drawable,
    const bafx::core::MonotonicTime startedAt,
    const bafx::core::MonotonicTime minimumPeriod) noexcept
{
    lastPresentedDrawableContent_ = drawable;
    if (minimumPeriod <= bafx::core::MonotonicTime::zero())
    {
        nextFramePacingDeadline_.reset();
        return;
    }

    const bafx::core::MonotonicTime followingDeadline =
        nextFramePacingDeadline_.has_value()
        ? *nextFramePacingDeadline_ + minimumPeriod
        : startedAt + minimumPeriod;
    // A delayed display must not submit a burst to catch up with missed ticks.
    nextFramePacingDeadline_ = followingDeadline > startedAt
        ? followingDeadline
        : startedAt + minimumPeriod;
}

void DisplaySession::resetFramePacing() noexcept
{
    nextFramePacingDeadline_.reset();
}

void DisplaySession::markRenderFaulted() noexcept
{
    renderFaulted_ = true;
}

void DisplaySession::clearRenderFault() noexcept
{
    renderFaulted_ = false;
}

void DisplaySession::show()
{
    window_.show();
}

DisplaySessionDeviceRecoveryResult DisplaySession::finishDeviceRecovery(
    const bafx::windows::GraphicsDeviceInfo& previousDeviceInfo,
    const bool backgroundWasActive) noexcept
{
    // Device recreation publishes a new blank swap chain even when authored
    // simulation state still exists; only a later successful Present can set it.
    lastPresentedDrawableContent_ = false;
    resetFramePacing();
    const bool adapterChanged =
        previousDeviceInfo.adapterLuid.LowPart
            != renderer_.deviceInfo().adapterLuid.LowPart
        || previousDeviceInfo.adapterLuid.HighPart
            != renderer_.deviceInfo().adapterLuid.HighPart;
    DisplaySessionDeviceRecoveryResult result{
        true,
        adapterChanged,
        backgroundWasActive,
        DisplaySessionBackgroundRecoveryStatus::NotRequired};
    if (secondaryBackgroundCapture_ == nullptr || !backgroundWasActive)
    {
        return result;
    }

    DisplaySessionBackgroundCaptureState& state =
        *secondaryBackgroundCapture_;
    state.captureSizeTracker.reset();
    const bafx::windows::WgcBackgroundStopDiagnostics stopDiagnostics =
        appendBackgroundCaptureStopDiagnostics(
            state.logPath,
            renderer_,
            "secondary-device-recovery");
    const bool retryEligible =
        stopDiagnostics.overallSucceeded
        && canRetryBackgroundCaptureAfterDeviceRecovery(
            state.request.sensorRequired,
            backgroundWasActive,
            adapterChanged,
            renderer_.deviceInfo().driverType,
            renderer_.backgroundCaptureRestartAllowed());
    if (!retryEligible)
    {
        result.background = DisplaySessionBackgroundRecoveryStatus::Blocked;
        return result;
    }

    if (state.execution.transactionActive)
    {
        // Let the transaction that already owns this surface finish first. Its
        // normal completion path will issue the monotonic retry token.
        state.sensorWasActiveBeforeTransaction = true;
        state.execution.deviceRecovered = true;
        state.execution.deviceRecoveryAdapterChanged = adapterChanged;
        result.background = DisplaySessionBackgroundRecoveryStatus::Queued;
        return result;
    }
    if (state.transition.transitioning())
    {
        // The intent exists but executeBackgroundCaptureTransition has not yet
        // initialized its result, which clears recovery flags. Preserve the
        // fact separately and merge it at that transaction's completion.
        state.rendererRecoveryPending = true;
        state.rendererRecoveryAdapterChanged = adapterChanged;
        state.rendererRecoveryBackgroundWasActive = backgroundWasActive;
        result.background = DisplaySessionBackgroundRecoveryStatus::Queued;
        return result;
    }

    if (state.request.retryToken
        == (std::numeric_limits<std::uint64_t>::max)())
    {
        result.background = DisplaySessionBackgroundRecoveryStatus::Blocked;
        return result;
    }

    try
    {
        ++state.request.retryToken;
        state.sensorWasActiveBeforeTransaction = false;
        requireStartedRequest(state.transition.beginRequest(state.request));
        state.outcomePending = true;
        result.background = DisplaySessionBackgroundRecoveryStatus::Queued;
    }
    catch (...)
    {
        // Device recovery succeeded, so preserve the FX surface and retire
        // only the optional capture owner when its retry cannot be represented.
        shutdownSecondaryBackgroundCapture();
        result.background = DisplaySessionBackgroundRecoveryStatus::Blocked;
    }
    return result;
}

void DisplaySession::acceptPendingSecondaryTargetIfApplied(
    DisplaySessionBackgroundCaptureState& state)
{
    if (!state.pendingTarget.has_value())
    {
        return;
    }

    if (!displayTargetBoundsApplied(state.execution))
    {
        // Once the owning transaction has ended, an unapplied intent has no
        // resource state to preserve. Retaining it would make topology
        // reconciliation mistake a failed migration for committed progress.
        state.pendingTarget.reset();
        state.pendingTargetOutputPreference.reset();
        state.pendingTargetColorCapabilities.reset();
        state.pendingWakeWindow = nullptr;
        return;
    }

    const bafx::windows::CompositionOutputPreference previousPreference =
        state.pendingTargetOutputPreference.value_or(
            renderer_.outputPreference());
    const std::optional<bafx::windows::DisplayColorCapabilities>
        previousCapabilities = state.pendingTargetColorCapabilities;
    acceptAppliedTarget(
        std::move(*state.pendingTarget),
        state.pendingWakeWindow);
    lastPresentedDrawableContent_ = false;
    resetFramePacing();
    // The coordinator refreshes after comparing old/new modes. Secondary
    // sessions have no separate comparison owner, so refresh at commit time.
    static_cast<void>(refreshColorCapabilities(previousCapabilities));
    const bafx::windows::CompositionOutputPreference targetPreference =
        resolveDisplayOutputPreference(
            requestedOutputPreference_,
            colorCapabilities_);
    const bool outputContractChanged = displayOutputContractChanged(
        previousPreference,
        targetPreference,
        previousCapabilities,
        colorCapabilities_);
    if (renderer_.outputPreference() != targetPreference
        || outputContractChanged)
    {
        // WGC textures share the output device. Queue the existing serialized
        // stop/recreate/restart path instead of replacing resources inline.
        state.pendingOutputRenegotiation =
            PendingSecondaryOutputRenegotiation{
                targetPreference,
                "display-target",
                target_};
    }
    else
    {
        // The retarget already rebuilt or reaffirmed this transport on the new
        // monitor. Any older color/configuration request is now redundant.
        state.pendingOutputRenegotiation.reset();
    }
    state.pendingTarget.reset();
    state.pendingTargetOutputPreference.reset();
    state.pendingTargetColorCapabilities.reset();
    state.pendingWakeWindow = nullptr;
}

std::optional<LUID> DisplaySession::requestedAdapter(
    const DisplayTarget& target) noexcept
{
    return target.sourceAdapterResolved
        ? std::optional<LUID>(target.sourceAdapterLuid)
        : std::nullopt;
}

}
