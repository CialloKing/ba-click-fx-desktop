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
    std::optional<DisplayTarget> pendingTarget{};
    std::optional<PendingSecondaryOutputRenegotiation>
        pendingOutputRenegotiation{};
    HWND pendingWakeWindow{nullptr};
    bool outcomePending{false};
    bool sensorWasActiveBeforeTransaction{false};
    bool rendererRecoveryPending{false};
    bool rendererRecoveryAdapterChanged{false};
    bool rendererRecoveryBackgroundWasActive{false};
};

namespace
{

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
    : target_(std::move(options.target)),
      window_(
          options.instance,
          target_.bounds,
          options.title,
          bafx::windows::OverlayWindowOptions::renderSurface()),
      renderer_(
          window_.handle(),
          window_.size(),
          options.bloomSettings,
          options.backgroundStopObserver,
          requestedAdapter(target_),
          options.outputPreference),
      simulation_(options.simulationSeed)
{
    colorMonitorStartResult_ = colorMonitor_.start(
        target_.monitor,
        options.wakeWindow);
    refreshColorCapabilities();
}

DisplaySession::~DisplaySession()
{
    shutdownSecondaryBackgroundCapture();
}

const DisplayTarget& DisplaySession::target() const noexcept
{
    return target_;
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

const bafx::windows::DisplayColorMonitorResult&
DisplaySession::colorMonitorStartResult() const noexcept
{
    return colorMonitorStartResult_;
}

bool DisplaySession::renderFaulted() const noexcept
{
    return renderFaulted_;
}

bool DisplaySession::lastPresentedDrawableContent() const noexcept
{
    return lastPresentedDrawableContent_;
}

void DisplaySession::acceptAppliedTarget(
    DisplayTarget target,
    const HWND wakeWindow) noexcept
{
    target_ = std::move(target);
    colorMonitorStartResult_ = colorMonitor_.start(
        target_.monitor,
        wakeWindow);
}

void DisplaySession::updateTargetMetadata(DisplayTarget target) noexcept
{
    target_ = std::move(target);
    // DPI and refresh-rate changes preserve the physical resource domain.
    // Refresh only the sample-age policy so a mixed-refresh desktop does not
    // inherit the previous cadence or pay for a WGC session restart.
    static_cast<void>(renderer_.refreshBackgroundCadence(target_.monitor));
}

DisplaySessionRetargetResult DisplaySession::retargetFxOnly(
    DisplayTarget target,
    const HWND wakeWindow)
{
    try
    {
        const DisplayOutputRetargetResult output = retargetDisplayOutput(
            window_,
            renderer_,
            DisplayOutputRetargetIntent{
                target.bounds,
                requestedAdapter(target),
                displayTargetSize(target)});
        lastPresentedDrawableContent_ = false;
        acceptAppliedTarget(std::move(target), wakeWindow);
        refreshColorCapabilities();
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

    state.pendingTarget = std::move(target);
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
        refreshColorCapabilities();
        state.pendingTarget.reset();
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
    const std::filesystem::path& logPath)
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
    requireStartedRequest(state->transition.beginRequest(request));
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
    state.sensorWasActiveBeforeTransaction =
        renderer_.backgroundCaptureActive();
    requireStartedRequest(state.transition.beginRequest(request));
    state.outcomePending = true;
    static_cast<void>(serviceSecondaryBackgroundCapture(
        bafx::core::MonotonicTime::zero()));
}

void DisplaySession::requestSecondaryOutputRenegotiation(
    const bafx::windows::CompositionOutputPreference preference,
    const std::string_view reason)
{
    if (secondaryBackgroundCapture_ == nullptr)
    {
        throw std::logic_error(
            "Secondary background capture is not initialized");
    }

    // Collapse repeated OS notifications to the newest contract. The service
    // owner performs the mutation only after any earlier WGC transaction ends.
    secondaryBackgroundCapture_->pendingOutputRenegotiation =
        PendingSecondaryOutputRenegotiation{
            preference,
            std::string(reason)};
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
                    ? DisplayTargetIntent{*state.pendingTarget, true}
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

        if (state.pendingOutputRenegotiation.has_value())
        {
            const PendingSecondaryOutputRenegotiation pending =
                *state.pendingOutputRenegotiation;
            state.pendingOutputRenegotiation.reset();
            result.outputRenegotiationPreference = pending.preference;
            result.outputRenegotiationReason = pending.reason;

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
                state.sensorWasActiveBeforeTransaction = false;
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
            if (const std::optional<bafx::windows::WindowSize> captureSize =
                    renderer_.pendingBackgroundFramePoolSize();
                captureSize.has_value())
            {
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
    if (!state.request.sensorRequired
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

void DisplaySession::refreshColorCapabilities() noexcept
{
    colorCapabilities_ = bafx::windows::queryDisplayColorCapabilities(
        target_.monitor);
}

void DisplaySession::recordPresentedDrawableContent(
    const bool drawable) noexcept
{
    lastPresentedDrawableContent_ = drawable;
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
    DisplaySessionBackgroundCaptureState& state) noexcept
{
    if (!state.pendingTarget.has_value()
        || !displayTargetBoundsApplied(state.execution))
    {
        return;
    }

    acceptAppliedTarget(
        std::move(*state.pendingTarget),
        state.pendingWakeWindow);
    lastPresentedDrawableContent_ = false;
    // The coordinator refreshes after comparing old/new modes. Secondary
    // sessions have no separate comparison owner, so refresh at commit time.
    refreshColorCapabilities();
    state.pendingTarget.reset();
    state.pendingWakeWindow = nullptr;
}

std::optional<LUID> DisplaySession::requestedAdapter(
    const DisplayTarget& target) noexcept
{
    return target.sourceIdentityResolved
        ? std::optional<LUID>(target.sourceAdapterLuid)
        : std::nullopt;
}

}
