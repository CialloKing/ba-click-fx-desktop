#include "background_capture_runtime.hpp"

#include "bafx/windows/overlay_window.hpp"

#include <chrono>
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
    case BackgroundCaptureFailure::FramePoolRecreateFailed:
        return "frame-pool-recreate-failed";
    case BackgroundCaptureFailure::InclusionUnconfirmed:
        return "inclusion-unconfirmed";
    case BackgroundCaptureFailure::SessionStopped:
        return "session-stopped";
    }
    return "unknown";
}

[[nodiscard]] std::string_view backgroundCaptureActionName(
    const bafx::windows::BackgroundCaptureActionKind kind) noexcept
{
    using bafx::windows::BackgroundCaptureActionKind;
    switch (kind)
    {
    case BackgroundCaptureActionKind::StopSensor:
        return "stop-sensor";
    case BackgroundCaptureActionKind::ResizeOutput:
        return "resize-output";
    case BackgroundCaptureActionKind::RecreateFramePool:
        return "recreate-frame-pool";
    case BackgroundCaptureActionKind::SetAffinityExcluded:
        return "set-affinity-excluded";
    case BackgroundCaptureActionKind::SetAffinityIncluded:
        return "set-affinity-included";
    case BackgroundCaptureActionKind::ApplyOverlayProfile:
        return "apply-overlay-profile";
    case BackgroundCaptureActionKind::StartSensor:
        return "start-sensor";
    }
    return "unknown";
}

void appendBackgroundCaptureActionBegin(
    const std::filesystem::path& logPath,
    const std::size_t index,
    const bafx::windows::BackgroundCaptureActionKind kind)
{
    std::string message = "BackgroundCapture.Action.Begin=";
    message += backgroundCaptureActionName(kind);
    message += ";Index=";
    message += std::to_string(index);
    bafx::windows::appendDiagnosticLog(logPath, message);
}

void appendBackgroundCaptureActionEnd(
    const std::filesystem::path& logPath,
    const std::size_t index,
    const bafx::windows::BackgroundCaptureActionKind kind,
    const bool succeeded,
    const std::chrono::steady_clock::duration elapsed)
{
    std::string message = "BackgroundCapture.Action.End=";
    message += backgroundCaptureActionName(kind);
    message += ";Index=";
    message += std::to_string(index);
    message += ";Succeeded=";
    message += succeeded ? "true" : "false";
    message += ";ElapsedUs=";
    message += std::to_string(
        std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count());
    bafx::windows::appendDiagnosticLog(logPath, message);
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

void appendBackgroundCaptureResourceLedger(
    const std::filesystem::path& logPath,
    const bafx::windows::CompositionRenderer& renderer,
    const std::string_view phase) noexcept
{
    try
    {
        const std::string ledgerDiagnostic =
            bafx::windows::wgcBackgroundResourceLedgerDiagnostic(
                renderer.backgroundResourceLedger());
        std::string message = "BackgroundCapture.ResourceLedger.Phase=";
        message += phase;
        message += ";";
        message += ledgerDiagnostic;
        bafx::windows::appendDiagnosticLog(logPath, message);
    }
    catch (...)
    {
        // A diagnostic failure must never prevent the owner from completing
        // the WGC cleanup it has already requested.
        bafx::windows::appendDiagnosticLog(
            logPath,
            "BackgroundCapture.ResourceLedger.Phase="
            "unknown;WGC.ResourceLedger=unavailable;Reason=formatter-failed");
    }
}

BackgroundCaptureExecutionResult executeBackgroundCaptureTransition(
    bafx::windows::BackgroundCaptureTransition& transition,
    bafx::windows::OverlayWindow& window,
    bafx::windows::CompositionRenderer& renderer,
    const HMONITOR monitor,
    const std::filesystem::path& logPath)
{
    BackgroundCaptureExecutionResult result{};
    const auto transactionStartedAt = std::chrono::steady_clock::now();
    std::size_t executedActionCount = 0U;
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

        appendBackgroundCaptureActionBegin(logPath, index, action->kind);
        const auto actionStartedAt = std::chrono::steady_clock::now();
        bool succeeded = false;
        try
        {
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
                    == bafx::windows::BackgroundCaptureActionKind::
                        SetAffinityExcluded;
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
            case bafx::windows::BackgroundCaptureActionKind::RecreateFramePool:
                succeeded = renderer.tryRecreateBackgroundFramePool(
                    action->captureSize);
                if (succeeded)
                {
                    result.recreatedFramePoolSize = action->captureSize;
                }
                else if (!renderer.backgroundCaptureFailure().empty())
                {
                    result.sensorFailure = renderer.backgroundCaptureFailure();
                }
                break;
            case bafx::windows::BackgroundCaptureActionKind::StartSensor:
                // Start is emitted only after WDA exclusion was confirmed in
                // this transaction, so stale affinity cannot enable capture.
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
        }
        catch (...)
        {
            appendBackgroundCaptureActionEnd(
                logPath,
                index,
                action->kind,
                false,
                std::chrono::steady_clock::now() - actionStartedAt);
            throw;
        }
        appendBackgroundCaptureActionEnd(
            logPath,
            index,
            action->kind,
            succeeded,
            std::chrono::steady_clock::now() - actionStartedAt);
        ++executedActionCount;

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

    std::string completion = "BackgroundCapture.Transaction.End;Actions=";
    completion += std::to_string(executedActionCount);
    completion += ";ElapsedUs=";
    completion += std::to_string(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - transactionStartedAt).count());
    bafx::windows::appendDiagnosticLog(logPath, completion);
    // Keep cumulative WGC ownership evidence beside every transaction.  A
    // failed stop/recreate can otherwise look successful after the sensor
    // pointer is released while an old WinRT resource is still live.
    appendBackgroundCaptureResourceLedger(logPath, renderer, "transaction");
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
        if (execution.recreatedFramePoolSize.has_value())
        {
            std::string message = "WGC frame pool recreated; size=";
            message += std::to_string(execution.recreatedFramePoolSize->width);
            message += "x";
            message += std::to_string(execution.recreatedFramePoolSize->height);
            bafx::windows::appendDiagnosticLog(logPath, message);
        }
        else
        {
            bafx::windows::appendDiagnosticLog(
                logPath,
                backgroundCaptureCapabilitiesDiagnostic(renderer));
        }
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
