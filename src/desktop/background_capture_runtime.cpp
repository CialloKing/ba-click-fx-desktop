#include "background_capture_runtime.hpp"

#include "bafx/windows/overlay_window.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
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
    case BackgroundCaptureFailure::SensorStopFailed:
        return "sensor-stop-failed";
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

[[nodiscard]] bafx::windows::DiagnosticLevel snapshotInvalidationLevel(
    const bafx::windows::BackgroundSnapshotInvalidationReason reason) noexcept
{
    using bafx::windows::BackgroundSnapshotInvalidationReason;
    switch (reason)
    {
    case BackgroundSnapshotInvalidationReason::WgcDrainFailed:
    case BackgroundSnapshotInvalidationReason::WgcSessionStopped:
    case BackgroundSnapshotInvalidationReason::SensorStartFailed:
        return bafx::windows::DiagnosticLevel::Warning;
    case BackgroundSnapshotInvalidationReason::VisibleBatchEnded:
    case BackgroundSnapshotInvalidationReason::FxOnlyPathSelected:
    case BackgroundSnapshotInvalidationReason::FramePoolReconfigureRequired:
    case BackgroundSnapshotInvalidationReason::OutputResize:
    case BackgroundSnapshotInvalidationReason::DeviceResourcesReleased:
    case BackgroundSnapshotInvalidationReason::CaptureSessionReplaced:
    case BackgroundSnapshotInvalidationReason::CaptureDisabled:
    case BackgroundSnapshotInvalidationReason::SnapshotResourcesRecreated:
        return bafx::windows::DiagnosticLevel::Info;
    }
    return bafx::windows::DiagnosticLevel::Warning;
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

void observeDeviceRecovery(
    BackgroundCaptureExecutionResult& result,
    const bafx::windows::GraphicsDeviceInfo& previousDeviceInfo,
    bafx::windows::CompositionRenderer& renderer,
    const std::filesystem::path& logPath,
    const std::string_view eventName,
    bool& sensorRestartAllowed)
{
    result.deviceRecovered = true;
    result.deviceRecoveryAdapterChanged =
        previousDeviceInfo.adapterLuid.LowPart
            != renderer.deviceInfo().adapterLuid.LowPart
        || previousDeviceInfo.adapterLuid.HighPart
            != renderer.deviceInfo().adapterLuid.HighPart;
    sensorRestartAllowed =
        !result.deviceRecoveryAdapterChanged
        && renderer.deviceInfo().driverType
            == bafx::windows::GraphicsDriverType::Hardware;
    const bafx::windows::DeviceRecoveryDiagnostics diagnostics =
        renderer.deviceRecoveryDiagnostics();
    const std::string totalMicroseconds = std::to_string(
        std::chrono::duration_cast<std::chrono::microseconds>(
            diagnostics.total).count());
    const std::string backgroundStopMicroseconds = std::to_string(
        std::chrono::duration_cast<std::chrono::microseconds>(
            diagnostics.backgroundStop).count());
    const std::array recoveryFields{
        bafx::windows::DiagnosticField{
            "Adapter",
            result.deviceRecoveryAdapterChanged ? "changed" : "same"},
        bafx::windows::DiagnosticField{
            "Driver",
            renderer.deviceInfo().driverType
                    == bafx::windows::GraphicsDriverType::Hardware
                ? "hardware"
                : "warp"},
        bafx::windows::DiagnosticField{
            "WgcRestartAllowed",
            sensorRestartAllowed ? "true" : "false"},
        bafx::windows::DiagnosticField{
            "WgcStopUs",
            backgroundStopMicroseconds},
        bafx::windows::DiagnosticField{
            "TotalUs",
            totalMicroseconds}};
    bafx::windows::appendDiagnosticEvent(
        logPath,
        eventName,
        recoveryFields);
    appendBackgroundCaptureStopDiagnostics(logPath, renderer, eventName);
}

}

bafx::windows::BackgroundCaptureRequest backgroundCaptureRequest(
    const bafx::config::Config& config,
    const std::uint64_t retryToken) noexcept
{
    return bafx::windows::BackgroundCaptureRequest{
        wantsBackgroundCapture(config),
        overlayProfileForRenderMode(config.background.mode),
        config.background.cursorExcluded,
        config.background.allowSystemBorder,
        retryToken};
}

bafx::windows::WgcBackgroundStopDiagnostics
appendBackgroundCaptureStopDiagnostics(
    const std::filesystem::path& logPath,
    bafx::windows::CompositionRenderer& renderer,
    const std::string_view phase) noexcept
{
    const bafx::windows::WgcBackgroundStopDiagnostics diagnostics =
        renderer.takeBackgroundStopDiagnostics();
    try
    {
        std::string message = "BackgroundCapture.Stop.Phase=";
        message += phase;
        message += ";";
        message += bafx::windows::wgcBackgroundStopDiagnostic(
            diagnostics);
        bafx::windows::appendDiagnosticLog(logPath, message);
    }
    catch (...)
    {
        bafx::windows::appendDiagnosticLog(
            logPath,
            "BackgroundCapture.Stop.Phase=unknown;WGC.Stop=unavailable;"
            "Reason=formatter-failed");
    }
    return diagnostics;
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
    bool sensorRestartAllowed = true;
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
                succeeded = appendBackgroundCaptureStopDiagnostics(
                    logPath,
                    renderer,
                    "transaction").overallSucceeded;
                if (!succeeded)
                {
                    sensorRestartAllowed = false;
                    result.sensorFailure =
                        "WGC stop failed; capture restart blocked for this process";
                }
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
            {
                const bafx::windows::GraphicsDeviceInfo previousDeviceInfo =
                    renderer.deviceInfo();
                const bafx::windows::OutputResizeStatus resizeStatus =
                    renderer.resizeOutput(action->outputSize);
                if (resizeStatus
                    == bafx::windows::OutputResizeStatus::DeviceRecovered)
                {
                    observeDeviceRecovery(
                        result,
                        previousDeviceInfo,
                        renderer,
                        logPath,
                        "Graphics.DeviceRecovery.ResizeSucceeded",
                        sensorRestartAllowed);
                }
                succeeded = true;
                break;
            }
            case bafx::windows::BackgroundCaptureActionKind::RecreateFramePool:
            {
                const bafx::windows::GraphicsDeviceInfo previousDeviceInfo =
                    renderer.deviceInfo();
                const bafx::windows::BackgroundFramePoolRecreateStatus
                    recreateStatus = renderer.tryRecreateBackgroundFramePool(
                    action->captureSize);
                switch (recreateStatus)
                {
                case bafx::windows::BackgroundFramePoolRecreateStatus::Recreated:
                    succeeded = true;
                    result.recreatedFramePoolSize = action->captureSize;
                    break;
                case bafx::windows::BackgroundFramePoolRecreateStatus::Failed:
                    succeeded = false;
                    break;
                case bafx::windows::BackgroundFramePoolRecreateStatus::
                    DeviceRecovered:
                    succeeded = false;
                    observeDeviceRecovery(
                        result,
                        previousDeviceInfo,
                        renderer,
                        logPath,
                        "Graphics.DeviceRecovery.FramePoolSucceeded",
                        sensorRestartAllowed);
                    break;
                case bafx::windows::BackgroundFramePoolRecreateStatus::
                    DeviceRecoveryFailed:
                    throw std::runtime_error(
                        "Graphics device recovery failed during WGC frame pool recreate: "
                        + std::string(renderer.deviceRecoveryFailure()));
                }
                if (!succeeded && !renderer.backgroundCaptureFailure().empty())
                {
                    result.sensorFailure = renderer.backgroundCaptureFailure();
                }
                break;
            }
            case bafx::windows::BackgroundCaptureActionKind::StartSensor:
                // Start is emitted only after WDA exclusion was confirmed in
                // this transaction, so stale affinity cannot enable capture.
                succeeded = sensorRestartAllowed
                    && renderer.tryEnableBackgroundCapture(
                        monitor,
                        true,
                        action->cursorExcluded,
                        action->allowSystemBorder);
                if (!sensorRestartAllowed)
                {
                    result.sensorFailure =
                        "WGC restart blocked after graphics adapter change or WARP recovery";
                }
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
            appendBackgroundCaptureResourceLedger(
                logPath,
                renderer,
                "action-failed");
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
            appendBackgroundCaptureResourceLedger(
                logPath,
                renderer,
                "transition-rejected");
            throw std::logic_error(
                "Background capture transition rejected its current action");
        }
    }
    if (transition.transitioning())
    {
        appendBackgroundCaptureResourceLedger(
            logPath,
            renderer,
            "budget-exceeded");
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

void appendBackgroundSnapshotInvalidation(
    const std::filesystem::path& logPath,
    const std::uint64_t controlGeneration,
    const bafx::windows::BackgroundSnapshotInvalidation& invalidation) noexcept
{
    try
    {
        const std::array values{
            std::to_string(controlGeneration),
            std::to_string(invalidation.frameId),
            std::to_string(invalidation.wgcEpoch),
            std::to_string(invalidation.wgcGeneration),
            std::to_string(invalidation.snapshotEpoch),
            std::to_string(invalidation.snapshotGeneration)};
        const std::array fields{
            bafx::windows::DiagnosticField{"Control.Generation", values[0]},
            bafx::windows::DiagnosticField{"Frame.Id", values[1]},
            bafx::windows::DiagnosticField{"WGC.Epoch", values[2]},
            bafx::windows::DiagnosticField{"WGC.Generation", values[3]},
            bafx::windows::DiagnosticField{
                "BackgroundSnapshot.Epoch",
                values[4]},
            bafx::windows::DiagnosticField{
                "BackgroundSnapshot.Generation",
                values[5]},
            bafx::windows::DiagnosticField{
                "BackgroundSnapshot.InvalidationReason",
                bafx::windows::backgroundSnapshotInvalidationReasonName(
                    invalidation.reason)}};
        bafx::windows::appendDiagnosticEvent(
            logPath,
            "BackgroundSnapshot.Invalidated",
            fields,
            snapshotInvalidationLevel(invalidation.reason));
    }
    catch (...)
    {
        // Diagnostics must never block the lifecycle action that invalidated
        // the snapshot or make an optional WGC path fatal.
    }
}

void appendBackgroundCompositeParticipation(
    const std::filesystem::path& logPath,
    const std::uint64_t controlGeneration,
    const bafx::windows::CompositionFrameDiagnostics& diagnostics) noexcept
{
    if (!diagnostics.backgroundParticipated)
    {
        return;
    }

    try
    {
        const std::array values{
            std::to_string(controlGeneration),
            std::to_string(diagnostics.frameId),
            std::to_string(diagnostics.wgc.epoch),
            std::to_string(diagnostics.wgc.acceptedGeneration),
            std::to_string(diagnostics.backgroundSnapshotEpoch),
            std::to_string(diagnostics.backgroundSnapshotGeneration)};
        const std::array fields{
            bafx::windows::DiagnosticField{"Control.Generation", values[0]},
            bafx::windows::DiagnosticField{"Frame.Id", values[1]},
            bafx::windows::DiagnosticField{"WGC.Epoch", values[2]},
            bafx::windows::DiagnosticField{"WGC.Generation", values[3]},
            bafx::windows::DiagnosticField{
                "BackgroundSnapshot.Epoch",
                values[4]},
            bafx::windows::DiagnosticField{
                "BackgroundSnapshot.Generation",
                values[5]}};
        bafx::windows::appendDiagnosticEvent(
            logPath,
            "BackgroundComposite.Participated",
            fields);
    }
    catch (...)
    {
        // Keep the compatibility marker available even if field formatting
        // could not allocate its temporary strings.
    }

    bafx::windows::appendDiagnosticLog(
        logPath,
        "WGC background sample entered the final desktop composite");
}

}
