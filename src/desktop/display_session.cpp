#include "display_session.hpp"
#include "background_capture_runtime.hpp"
#include "display_output_retarget.hpp"

#include <limits>
#include <stdexcept>
#include <utility>

namespace bafx::desktop
{
struct DisplaySessionBackgroundCaptureState final
{
    bafx::windows::BackgroundCaptureTransition transition{};
    BackgroundCaptureExecutionResult execution{};
    bafx::windows::BackgroundCaptureRequest request{};
    std::uint64_t controlGeneration{0U};
    std::filesystem::path logPath{};
    std::string pendingSensorFailure{};
    CaptureExclusionHealthPoller exclusionHealthPoller{};
    bool outcomePending{false};
    bool sensorWasActiveBeforeTransaction{false};
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
        acceptAppliedTarget(std::move(target), wakeWindow);
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
    const bafx::windows::BackgroundCaptureRequest request,
    const std::uint64_t controlGeneration)
{
    if (secondaryBackgroundCapture_ == nullptr)
    {
        throw std::logic_error(
            "Secondary background capture is not initialized");
    }

    DisplaySessionBackgroundCaptureState& state =
        *secondaryBackgroundCapture_;
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
                : DisplayTargetIntent{target_, false};
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
            result.deviceRecovered = result.deviceRecovered
                || state.execution.deviceRecovered;
            if (!state.pendingSensorFailure.empty()
                && state.execution.sensorFailure.empty())
            {
                state.execution.sensorFailure = state.pendingSensorFailure;
            }
            state.pendingSensorFailure.clear();
            appendSecondaryBackgroundOutcome(state, renderer_);
            if (state.execution.deviceRecovered)
            {
                const bool retryEligible =
                    canRetryBackgroundCaptureAfterDeviceRecovery(
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
            state.pendingSensorFailure = stoppedReason;
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

std::optional<LUID> DisplaySession::requestedAdapter(
    const DisplayTarget& target) noexcept
{
    return target.sourceIdentityResolved
        ? std::optional<LUID>(target.sourceAdapterLuid)
        : std::nullopt;
}

}
