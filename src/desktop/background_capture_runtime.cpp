#include "background_capture_runtime.hpp"

#include "bafx/windows/overlay_window.hpp"

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string_view>

namespace bafx::desktop
{
namespace
{

[[nodiscard]] bool wantsBackgroundCapture(
    const bafx::config::Config& config) noexcept
{
    return config.background.mode == bafx::config::RenderMode::BackgroundAware;
}

[[nodiscard]] bafx::windows::FxOverlayProfile overlayProfileForRenderMode(
    const bafx::config::RenderMode mode) noexcept
{
    switch (mode)
    {
    case bafx::config::RenderMode::RecordingCompatible:
        return bafx::windows::FxOverlayProfile::RecordingCompatible;
    case bafx::config::RenderMode::LightBackground:
        return bafx::windows::FxOverlayProfile::LightBackground;
    case bafx::config::RenderMode::BackgroundAware:
        // Background-aware falls back to the stable FX-only transport until
        // a captured sample enters the exact desktop composite.
        return bafx::windows::FxOverlayProfile::FxOnlyFallback;
    }
    return bafx::windows::FxOverlayProfile::FxOnlyFallback;
}

[[nodiscard]] std::string backgroundCaptureCapabilitiesDiagnostic(
    const bafx::windows::CompositionRenderer& renderer)
{
    std::string message = "WGC capture session active; system-border=";
    message += renderer.backgroundCaptureBorderHidden() ? "hidden" : "visible-allowed";
    message += "; cursor=";
    message += renderer.backgroundCaptureCursorExcluded() ? "excluded" : "captured";
    return message;
}

[[nodiscard]] std::string_view backgroundCaptureFailureName(
    const bafx::windows::BackgroundCaptureFailure failure) noexcept
{
    using bafx::windows::BackgroundCaptureFailure;
    switch (failure)
    {
    case BackgroundCaptureFailure::None:
        return "none";
    case BackgroundCaptureFailure::ExclusionUnconfirmed:
        return "exclusion-unconfirmed";
    case BackgroundCaptureFailure::SensorStartFailed:
        return "sensor-start-failed";
    case BackgroundCaptureFailure::InclusionUnconfirmed:
        return "inclusion-unconfirmed";
    case BackgroundCaptureFailure::SessionStopped:
        return "session-stopped";
    }
    return "unknown";
}

}

bafx::windows::BackgroundCaptureRequest backgroundCaptureRequest(
    const bafx::config::Config& config) noexcept
{
    return bafx::windows::BackgroundCaptureRequest{
        wantsBackgroundCapture(config),
        overlayProfileForRenderMode(config.background.mode),
        config.background.cursorExcluded,
        config.background.allowSystemBorder,
        0U};
}

BackgroundCaptureExecutionResult executeBackgroundCaptureTransition(
    bafx::windows::BackgroundCaptureTransition& transition,
    bafx::windows::OverlayWindow& window,
    bafx::windows::CompositionRenderer& renderer,
    const HMONITOR monitor,
    const std::filesystem::path& logPath)
{
    BackgroundCaptureExecutionResult result{};
    for (std::size_t index = 0U;
         index < bafx::windows::maximumBackgroundCaptureActions;
         ++index)
    {
        const std::optional<bafx::windows::BackgroundCaptureAction> action =
            transition.nextAction();
        if (!action.has_value())
        {
            break;
        }

        bool succeeded = false;
        switch (action->kind)
        {
        case bafx::windows::BackgroundCaptureActionKind::StopSensor:
            renderer.disableBackgroundCapture();
            succeeded = true;
            break;
        case bafx::windows::BackgroundCaptureActionKind::SetAffinityExcluded:
        case bafx::windows::BackgroundCaptureActionKind::SetAffinityIncluded:
        {
            const bool excluded = action->kind
                == bafx::windows::BackgroundCaptureActionKind::SetAffinityExcluded;
            const bafx::windows::CaptureExclusionStatus status =
                window.setCaptureExcluded(excluded);
            bafx::windows::appendDiagnosticLog(
                logPath,
                bafx::windows::captureExclusionDiagnostic(status));
            succeeded = status.confirmed();
            break;
        }
        case bafx::windows::BackgroundCaptureActionKind::ApplyOverlayProfile:
            renderer.setOverlayProfile(action->overlayProfile);
            succeeded = true;
            break;
        case bafx::windows::BackgroundCaptureActionKind::ResizeOutput:
            renderer.resizeOutput(action->outputSize);
            succeeded = true;
            break;
        case bafx::windows::BackgroundCaptureActionKind::StartSensor:
            // Start is emitted only after WDA exclusion was confirmed in this
            // transaction, so a stale affinity result cannot enable capture.
            succeeded = renderer.tryEnableBackgroundCapture(
                monitor,
                true,
                action->cursorExcluded,
                action->allowSystemBorder);
            if (!succeeded && !renderer.backgroundCaptureFailure().empty())
            {
                result.sensorFailure = renderer.backgroundCaptureFailure();
            }
            break;
        }

        if (!transition.applyObservation(*action, succeeded))
        {
            throw std::logic_error(
                "Background capture transition rejected its current action");
        }
    }
    if (transition.transitioning())
    {
        throw std::logic_error(
            "Background capture transition exceeded its fixed action budget");
    }
    return result;
}

bafx::windows::BackgroundCaptureStatus backgroundCaptureStatus(
    const bafx::windows::EffectiveBackgroundCapturePath path) noexcept
{
    using bafx::windows::BackgroundCaptureStatus;
    using bafx::windows::EffectiveBackgroundCapturePath;
    switch (path)
    {
    case EffectiveBackgroundCapturePath::BackgroundAware:
        return BackgroundCaptureStatus::Active;
    case EffectiveBackgroundCapturePath::FxOnly:
        return BackgroundCaptureStatus::FallbackFxOnly;
    case EffectiveBackgroundCapturePath::FxOnlyCaptureVisibilityUnknown:
        return BackgroundCaptureStatus::FallbackFxOnlyCaptureVisibilityUnknown;
    }
    return BackgroundCaptureStatus::FallbackFxOnlyCaptureVisibilityUnknown;
}

void appendBackgroundCaptureOutcome(
    const std::filesystem::path& logPath,
    const bafx::windows::BackgroundCaptureRequest& request,
    const bafx::windows::BackgroundCaptureTransition& transition,
    const BackgroundCaptureExecutionResult& execution,
    const bafx::windows::CompositionRenderer& renderer)
{
    if (transition.effectivePath()
        == bafx::windows::EffectiveBackgroundCapturePath::BackgroundAware)
    {
        bafx::windows::appendDiagnosticLog(
            logPath,
            backgroundCaptureCapabilitiesDiagnostic(renderer));
        return;
    }

    std::string message = request.sensorRequired
        ? "WGC background capture unavailable; using FX-only rendering"
        : "WGC background capture disabled by configuration; using FX-only rendering";
    if (transition.failure() != bafx::windows::BackgroundCaptureFailure::None)
    {
        message += "; transition-failure=";
        message += backgroundCaptureFailureName(transition.failure());
    }
    if (!execution.sensorFailure.empty())
    {
        message += "; reason=";
        message += execution.sensorFailure;
    }
    if (transition.effectivePath()
        == bafx::windows::EffectiveBackgroundCapturePath::
            FxOnlyCaptureVisibilityUnknown)
    {
        message += "; capture-visibility=unknown";
    }
    bafx::windows::appendDiagnosticLog(logPath, message);
}

}
