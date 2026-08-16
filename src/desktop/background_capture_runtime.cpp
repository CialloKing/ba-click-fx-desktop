#include "background_capture_runtime.hpp"
#include "display_output_retarget.hpp"

#include "bafx/windows/overlay_window.hpp"
#include "bafx/windows/package_identity.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <optional>
#include <sstream>
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

[[nodiscard]] std::string win32Hex(const DWORD value)
{
    std::ostringstream stream;
    stream << "0x" << std::hex << std::uppercase << std::setw(8)
           << std::setfill('0') << value;
    return stream.str();
}

[[nodiscard]] std::string backgroundCaptureCapabilitiesDiagnostic(
    const bafx::windows::CompositionRenderer& renderer)
{
    std::string message = "WGC capture session active; system-border=";
    message += renderer.backgroundCaptureBorderHidden() ? "hidden" : "visible-allowed";
    message += "; cursor=";
    message += renderer.backgroundCaptureCursorExcluded() ? "excluded" : "captured";
    const bafx::windows::WgcProducerCadenceState producer =
        renderer.backgroundCaptureProducerCadence();
    message += "; producer-cadence=";
    message += bafx::windows::wgcProducerCadenceStatusName(producer.status);
    message += "; producer-requested-us=";
    message += std::to_string(
        std::chrono::duration_cast<std::chrono::microseconds>(
            producer.requested).count());
    message += "; producer-applied-us=";
    message += std::to_string(
        std::chrono::duration_cast<std::chrono::microseconds>(
            producer.applied).count());
    message += "; producer-result=";
    message += win32Hex(static_cast<DWORD>(producer.result));
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
    case BackgroundCaptureFailure::BorderlessAccessFailed:
        return "borderless-access-failed";
    case BackgroundCaptureFailure::BorderlessAccessCanceled:
        return "borderless-access-canceled";
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
    case BackgroundCaptureFailure::CaptureSizeMismatch:
        return "capture-size-mismatch";
    }
    return "unknown";
}

[[nodiscard]] std::string_view backgroundCaptureActionName(
    const bafx::windows::BackgroundCaptureActionKind kind) noexcept
{
    using bafx::windows::BackgroundCaptureActionKind;
    switch (kind)
    {
    case BackgroundCaptureActionKind::RequestBorderlessAccess:
        return "request-borderless-access";
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
    case BackgroundSnapshotInvalidationReason::ReferenceWhiteUnavailable:
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

void beginBackgroundCaptureExecution(
    BackgroundCaptureExecutionResult& execution,
    const DisplayTargetIntent& targetIntent,
    const std::uint64_t controlGeneration,
    const std::chrono::steady_clock::time_point now)
{
    execution.sensorFailure.clear();
    execution.resizedOutputSize.reset();
    execution.recreatedFramePoolSize.reset();
    execution.deviceRecovered = false;
    execution.deviceRecoveryAdapterChanged = false;
    execution.outputAdapterRetargeted = false;
    execution.outputAdapterWarpFallback = false;
    execution.transactionActive = true;
    execution.pending = false;
    execution.sensorRestartAllowed = true;
    execution.borderlessAccessConfirmed = false;
    execution.targetIntent = targetIntent;
    execution.controlGeneration = controlGeneration;
    execution.actionIndex = 0U;
    execution.executedActionCount = 0U;
    execution.transactionStartedAt = now;
    execution.actionStartedAt = {};
    execution.activeAction.reset();
}

void beginBackgroundCaptureAction(
    BackgroundCaptureExecutionResult& execution,
    const bafx::windows::BackgroundCaptureAction& action,
    const std::filesystem::path& logPath)
{
    if (execution.activeAction.has_value())
    {
        if (*execution.activeAction != action)
        {
            throw std::logic_error(
                "Background capture action changed while it was pending");
        }
        return;
    }

    execution.activeAction = action;
    execution.actionStartedAt = std::chrono::steady_clock::now();
    appendBackgroundCaptureActionBegin(
        logPath,
        execution.actionIndex,
        action.kind);
}

void appendBackgroundCaptureTransactionTarget(
    const std::filesystem::path& logPath,
    const BackgroundCaptureExecutionResult& execution) noexcept
{
    try
    {
        const std::string generation = std::to_string(
            execution.controlGeneration);
        const std::string monitor = formatDisplayTargetMonitor(
            execution.targetIntent.target);
        const std::string device = displayTargetDeviceUtf8(
            execution.targetIntent.target);
        const std::string bounds = formatDisplayTargetBounds(
            execution.targetIntent.target);
        const std::array fields{
            bafx::windows::DiagnosticField{
                "Control.Generation",
                generation},
            bafx::windows::DiagnosticField{
                "Display.Target.Monitor",
                monitor},
            bafx::windows::DiagnosticField{
                "Display.Target.Device",
                device},
            bafx::windows::DiagnosticField{
                "Display.Target.Bounds",
                bounds},
            bafx::windows::DiagnosticField{
                "Display.Target.ApplyBounds",
                execution.targetIntent.applyBounds ? "true" : "false"}};
        bafx::windows::appendDiagnosticEvent(
            logPath,
            "BackgroundCapture.Transaction.Begin",
            fields);
    }
    catch (...)
    {
        // Target diagnostics must not prevent the already-started owner
        // transaction from reaching its finite cleanup path.
        bafx::windows::appendDiagnosticLog(
            logPath,
            "BackgroundCapture.Transaction.Begin=diagnostic-unavailable");
    }
}

void finishBackgroundCaptureAction(
    bafx::windows::BackgroundCaptureTransition& transition,
    BackgroundCaptureExecutionResult& execution,
    const bafx::windows::BackgroundCaptureActionObservation observation,
    const std::filesystem::path& logPath)
{
    if (!execution.activeAction.has_value()
        || observation == bafx::windows::BackgroundCaptureActionObservation::Pending)
    {
        throw std::logic_error(
            "Background capture action completion was not terminal");
    }

    const bafx::windows::BackgroundCaptureAction action =
        *execution.activeAction;
    appendBackgroundCaptureActionEnd(
        logPath,
        execution.actionIndex,
        action.kind,
        observation
            == bafx::windows::BackgroundCaptureActionObservation::Succeeded,
        std::chrono::steady_clock::now() - execution.actionStartedAt);
    if (!transition.applyObservation(action, observation))
    {
        throw std::logic_error(
            "Background capture transition rejected its current action");
    }
    ++execution.actionIndex;
    ++execution.executedActionCount;
    execution.activeAction.reset();
    execution.pending = false;
}

void appendBackgroundCaptureCancellation(
    const std::filesystem::path& logPath,
    const BackgroundCaptureExecutionResult& execution,
    const BackgroundCaptureCancelResizePolicy resizePolicy,
    const std::string_view reason) noexcept
{
    try
    {
        const std::array values{
            std::to_string(execution.controlGeneration),
            std::to_string(execution.actionIndex)};
        const std::array fields{
            bafx::windows::DiagnosticField{"Control.Generation", values[0]},
            bafx::windows::DiagnosticField{
                "Transaction.ActionIndex",
                values[1]},
            bafx::windows::DiagnosticField{
                "PendingResize",
                resizePolicy == BackgroundCaptureCancelResizePolicy::Discard
                    ? "discard"
                    : "preserve"},
            bafx::windows::DiagnosticField{"Reason", reason}};
        bafx::windows::appendDiagnosticEvent(
            logPath,
            "BackgroundCapture.Transaction.Cancel",
            fields,
            bafx::windows::DiagnosticLevel::Warning);
    }
    catch (...)
    {
        // Diagnostics must not prevent an owner-requested broker cancellation.
    }
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
    const bafx::windows::WgcBackgroundStopDiagnostics stopDiagnostics =
        appendBackgroundCaptureStopDiagnostics(logPath, renderer, eventName);
    sensorRestartAllowed =
        !result.deviceRecoveryAdapterChanged
        && renderer.deviceInfo().driverType
            == bafx::windows::GraphicsDriverType::Hardware
        && renderer.backgroundCaptureRestartAllowed();
    if (!stopDiagnostics.overallSucceeded)
    {
        result.sensorFailure =
            "WGC stop failed; capture restart blocked for this process";
    }
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
}

}

bool CaptureExclusionHealthPoller::shouldQuery(
    const bool captureActive,
    const std::chrono::nanoseconds now) noexcept
{
    if (!captureActive)
    {
        lastObservedAt_.reset();
        return false;
    }
    if (!lastObservedAt_.has_value() || now < *lastObservedAt_)
    {
        // Starting a sensor already performs a set/readback transaction. Delay
        // the first health query, and restart the cadence after clock regression.
        lastObservedAt_ = now;
        return false;
    }
    if (now - *lastObservedAt_
        < backgroundCaptureExclusionHealthInterval)
    {
        return false;
    }

    // Missed periods collapse into one query so a stalled Host cannot emit a
    // burst of Win32 calls while it is recovering.
    lastObservedAt_ = now;
    return true;
}

BackgroundCaptureCancelResizePolicy backgroundCaptureCancelResizePolicy(
    const bool outputResizeSupersedes,
    const bool displayTargetSupersedes) noexcept
{
    // An owner change alone must not lose geometry already consumed from the
    // Win32 queue. Only a newer geometry intent can replace that resize.
    return outputResizeSupersedes || displayTargetSupersedes
        ? BackgroundCaptureCancelResizePolicy::Discard
        : BackgroundCaptureCancelResizePolicy::Preserve;
}

bool displayTargetBoundsApplied(
    const BackgroundCaptureExecutionResult& execution) noexcept
{
    if (!execution.targetIntent.applyBounds
        || !execution.resizedOutputSize.has_value())
    {
        return false;
    }
    const bafx::windows::WindowSize expected = displayTargetSize(
        execution.targetIntent.target);
    return execution.resizedOutputSize->width == expected.width
        && execution.resizedOutputSize->height == expected.height;
}

BackgroundCaptureStopMonitor::BackgroundCaptureStopMonitor(
    const std::filesystem::path& logPath,
    const std::chrono::milliseconds timeout,
    const BackgroundCaptureStopTimeoutHandler timeoutHandler,
    const void* const timeoutContext)
    : logPath_(logPath),
      watchdog_(timeout, timeoutHandler, timeoutContext)
{
}

bafx::windows::WgcBackgroundStopObserver
BackgroundCaptureStopMonitor::observer() noexcept
{
    return bafx::windows::WgcBackgroundStopObserver{
        this,
        &BackgroundCaptureStopMonitor::observe};
}

void BackgroundCaptureStopMonitor::observe(
    const void* const context,
    const bafx::windows::WgcBackgroundStopProgress& progress) noexcept
{
    if (context == nullptr)
    {
        return;
    }
    auto& monitor = *static_cast<BackgroundCaptureStopMonitor*>(
        const_cast<void*>(context));
    monitor.record(progress);
}

void BackgroundCaptureStopMonitor::record(
    const bafx::windows::WgcBackgroundStopProgress& progress) noexcept
{
    bool armRejected = false;
    std::string_view armStatus = "not-requested";
    if (progress.stage == bafx::windows::WgcBackgroundStopStage::Stop)
    {
        if (progress.state
            == bafx::windows::WgcBackgroundStopStageState::Begin)
        {
            // Arm before touching the filesystem. A blocked diagnostic write
            // must not remove the hard process boundary from WGC teardown.
            const bool accepted = watchdog_.arm();
            armRejected = !accepted;
            armStatus = accepted ? "accepted" : "rejected";
        }
        else
        {
            // The WinRT teardown has returned. Do not let slow logging turn a
            // completed stop into a false watchdog termination.
            watchdog_.disarm();
        }
    }

    try
    {
        const std::string ownerThreadId = std::to_string(progress.ownerThreadId);
        const std::string callerThreadId = std::to_string(progress.callerThreadId);
        const std::string timeoutMilliseconds = std::to_string(
            watchdog_.timeout().count());
        const std::array fields{
            bafx::windows::DiagnosticField{
                "WGC.Stop.Stage",
                bafx::windows::wgcBackgroundStopStageName(progress.stage)},
            bafx::windows::DiagnosticField{
                "WGC.Stop.StageState",
                bafx::windows::wgcBackgroundStopStageStateName(progress.state)},
            bafx::windows::DiagnosticField{
                "WGC.Stop.OwnerThreadId",
                ownerThreadId},
            bafx::windows::DiagnosticField{
                "WGC.Stop.CallerThreadId",
                callerThreadId},
            bafx::windows::DiagnosticField{
                "WGC.Stop.OwnerThreadMatched",
                progress.ownerThreadMatched() ? "true" : "false"},
            bafx::windows::DiagnosticField{
                "WGC.Stop.WatchdogArmed",
                watchdog_.armed() ? "true" : "false"},
            bafx::windows::DiagnosticField{
                "WGC.Stop.WatchdogArmStatus",
                armStatus},
            bafx::windows::DiagnosticField{
                "WGC.Stop.WatchdogTimeoutMs",
                timeoutMilliseconds}};
        const bool warning = !progress.ownerThreadMatched()
            || armRejected
            || progress.state
                == bafx::windows::WgcBackgroundStopStageState::Failed;
        bafx::windows::appendDiagnosticEvent(
            logPath_,
            "BackgroundCapture.StopProgress",
            fields,
            warning
                ? bafx::windows::DiagnosticLevel::Warning
                : bafx::windows::DiagnosticLevel::Info);
    }
    catch (...)
    {
        // Diagnostics must not disarm or delay the hard teardown deadline.
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

bool canRetryBackgroundCaptureAfterDeviceRecovery(
    const bool captureRequested,
    const bool sensorWasActive,
    const bool adapterChanged,
    const bafx::windows::GraphicsDriverType driverType,
    const bool rendererRestartAllowed) noexcept
{
    // Recovery may replace only a Sensor that belonged to the failed device.
    // A stable FX-only fallback must retain its terminal request identity.
    return captureRequested
        && sensorWasActive
        && !adapterChanged
        && driverType == bafx::windows::GraphicsDriverType::Hardware
        && rendererRestartAllowed;
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

void appendBorderlessCaptureAccessCheck(
    const std::filesystem::path& logPath,
    const std::uint64_t controlGeneration,
    const std::size_t actionIndex,
    const bafx::windows::BorderlessCaptureAccessResult& result) noexcept
{
    try
    {
        std::ostringstream errorStream;
        errorStream << "0x" << std::hex << std::uppercase << std::setw(8)
                    << std::setfill('0')
                    << static_cast<unsigned long>(result.error);
        std::ostringstream capabilityErrorStream;
        if (result.capability.has_value())
        {
            capabilityErrorStream
                << "0x" << std::hex << std::uppercase << std::setw(8)
                << std::setfill('0')
                << static_cast<unsigned long>(result.capability->error);
        }
        else
        {
            capabilityErrorStream << "not-checked";
        }
        std::ostringstream trustErrorStream;
        if (result.externalHostTrust.has_value())
        {
            trustErrorStream
                << "0x" << std::hex << std::uppercase << std::setw(8)
                << std::setfill('0')
                << static_cast<unsigned long>(result.externalHostTrust->error);
        }
        else
        {
            trustErrorStream << "not-checked";
        }
        const std::array values{
            std::to_string(controlGeneration),
            std::to_string(actionIndex),
            errorStream.str(),
            std::to_string(result.elapsedMilliseconds),
            capabilityErrorStream.str(),
            std::to_string(
                bafx::windows::borderlessCaptureUniversalApiContractVersion),
            trustErrorStream.str()};
        const auto presence = [&result](const bool present) noexcept
            -> std::string_view
        {
            return result.capability.has_value()
                ? (present ? "true" : "false")
                : "not-checked";
        };
        const std::array fields{
            bafx::windows::DiagnosticField{"Control.Generation", values[0]},
            bafx::windows::DiagnosticField{
                "Transaction.ActionIndex",
                values[1]},
            bafx::windows::DiagnosticField{
                "Background.AllowSystemBorder",
                "false"},
            bafx::windows::DiagnosticField{
                "WGC.BorderlessAccess.Status",
                bafx::windows::borderlessCaptureAccessStatusName(result.status)},
            bafx::windows::DiagnosticField{
                "WGC.BorderlessAccess.HRESULT",
                values[2]},
            bafx::windows::DiagnosticField{
                "WGC.BorderlessAccess.AsyncStatus",
                bafx::windows::borderlessCaptureAccessAsyncStatusName(
                    result.asyncStatus)},
            bafx::windows::DiagnosticField{
                "WGC.BorderlessAccess.ElapsedMs",
                values[3]},
            bafx::windows::DiagnosticField{
                "WGC.BorderlessAccess.CancelRequested",
                result.cancelRequested ? "true" : "false"},
            bafx::windows::DiagnosticField{
                "WGC.BorderlessAccess.Allowed",
                bafx::windows::borderlessCaptureAccessAllowed(result)
                    ? "true"
                    : "false"},
            bafx::windows::DiagnosticField{
                "WGC.BorderlessAccess.Capability.Status",
                result.capability.has_value()
                    ? bafx::windows::borderlessCaptureCapabilityStatusName(
                          result.capability->status)
                    : "not-checked"},
            bafx::windows::DiagnosticField{
                "WGC.BorderlessAccess.Capability.HRESULT",
                values[4]},
            bafx::windows::DiagnosticField{
                "WGC.BorderlessAccess.Capability.ContractVersion",
                values[5]},
            bafx::windows::DiagnosticField{
                "WGC.BorderlessAccess.Capability.ContractPresent",
                presence(
                    result.capability.has_value()
                        && result.capability->universalApiContractV12)},
            bafx::windows::DiagnosticField{
                "WGC.BorderlessAccess.Capability.AccessTypePresent",
                presence(
                    result.capability.has_value()
                        && result.capability->graphicsCaptureAccessType)},
            bafx::windows::DiagnosticField{
                "WGC.BorderlessAccess.Capability.RequestMethodPresent",
                presence(
                    result.capability.has_value()
                        && result.capability->requestAccessAsyncMethod)},
            bafx::windows::DiagnosticField{
                "WGC.BorderlessAccess.Capability.BorderlessKindPresent",
                presence(
                    result.capability.has_value()
                        && result.capability->borderlessAccessKind)},
            bafx::windows::DiagnosticField{
                "WGC.BorderlessAccess.Capability.BorderPropertyWriteable",
                presence(
                    result.capability.has_value()
                        && result.capability->isBorderRequiredProperty)},
            bafx::windows::DiagnosticField{
                "WGC.BorderlessAccess.ExternalHostTrust.Status",
                result.externalHostTrust.has_value()
                    ? bafx::windows::externalHostTrustStatusName(
                          result.externalHostTrust->status)
                    : "not-checked"},
            bafx::windows::DiagnosticField{
                "WGC.BorderlessAccess.ExternalHostTrust.HRESULT",
                values[6]},
            bafx::windows::DiagnosticField{
                "WGC.BorderlessAccess.ExternalHostTrust.StatePath",
                result.externalHostTrust.has_value()
                    ? std::string_view(result.externalHostTrust->statePath)
                    : std::string_view("not-checked")},
            bafx::windows::DiagnosticField{
                "WGC.BorderlessAccess.ExternalHostTrust.ExpectedHostSha256",
                result.externalHostTrust.has_value()
                    ? std::string_view(
                          result.externalHostTrust->expectedHostSha256)
                    : std::string_view("not-checked")},
            bafx::windows::DiagnosticField{
                "WGC.BorderlessAccess.ExternalHostTrust.ObservedHostSha256",
                result.externalHostTrust.has_value()
                    ? std::string_view(
                          result.externalHostTrust->observedHostSha256)
                    : std::string_view("not-checked")},
            bafx::windows::DiagnosticField{
                "WGC.BorderlessAccess.ExternalHostTrust.ExpectedPackageSha256",
                result.externalHostTrust.has_value()
                    ? std::string_view(
                          result.externalHostTrust->expectedPackageSha256)
                    : std::string_view("not-checked")},
            bafx::windows::DiagnosticField{
                "WGC.BorderlessAccess.ExternalHostTrust.ObservedPackageSha256",
                result.externalHostTrust.has_value()
                    ? std::string_view(
                          result.externalHostTrust->observedPackageSha256)
                    : std::string_view("not-checked")},
            bafx::windows::DiagnosticField{
                "WGC.BorderlessAccess.ExternalHostTrust.ExpectedCertificateSha256",
                result.externalHostTrust.has_value()
                    ? std::string_view(
                          result.externalHostTrust->expectedCertificateSha256)
                    : std::string_view("not-checked")},
            bafx::windows::DiagnosticField{
                "WGC.BorderlessAccess.ExternalHostTrust.ObservedCertificateSha256",
                result.externalHostTrust.has_value()
                    ? std::string_view(
                          result.externalHostTrust->observedCertificateSha256)
                    : std::string_view("not-checked")}};
        bafx::windows::appendDiagnosticEvent(
            logPath,
            "WGC.BorderlessAccess.Checked",
            fields,
            bafx::windows::borderlessCaptureAccessAllowed(result)
                ? bafx::windows::DiagnosticLevel::Info
                : bafx::windows::DiagnosticLevel::Warning);
    }
    catch (...)
    {
        // Permission diagnostics cannot make an optional capture path fatal.
    }
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

BackgroundCaptureExecutionStatus executeBackgroundCaptureTransition(
    bafx::windows::BackgroundCaptureTransition& transition,
    bafx::windows::OverlayWindow& window,
    bafx::windows::CompositionRenderer& renderer,
    const DisplayTargetIntent& targetIntent,
    const std::uint64_t controlGeneration,
    bafx::windows::BorderlessCaptureAccessAuthority& borderlessAccessAuthority,
    BackgroundCaptureExecutionResult& execution,
    const std::filesystem::path& logPath)
{
    if (!execution.transactionActive)
    {
        if (!transition.transitioning())
        {
            return BackgroundCaptureExecutionStatus::Completed;
        }
        beginBackgroundCaptureExecution(
            execution,
            targetIntent,
            controlGeneration,
            std::chrono::steady_clock::now());
        appendBackgroundCaptureTransactionTarget(logPath, execution);
    }
    else if (execution.controlGeneration != controlGeneration
        || !sameDisplayTargetIntent(execution.targetIntent, targetIntent))
    {
        throw std::logic_error(
            "Background capture owner changed a pending transaction identity");
    }

    while (transition.transitioning())
    {
        if (execution.executedActionCount
            >= bafx::windows::maximumBackgroundCaptureActions)
        {
            appendBackgroundCaptureResourceLedger(
                logPath,
                renderer,
                "budget-exceeded");
            throw std::logic_error(
                "Background capture transition exceeded its fixed action budget");
        }

        const std::optional<bafx::windows::BackgroundCaptureAction> action =
            transition.nextAction();
        if (!action.has_value())
        {
            throw std::logic_error(
                "Background capture transition lost its current action");
        }

        beginBackgroundCaptureAction(execution, *action, logPath);
        bool succeeded = false;
        try
        {
            switch (action->kind)
            {
            case bafx::windows::BackgroundCaptureActionKind::
                RequestBorderlessAccess:
            {
                const bafx::windows::BorderlessCaptureAccessPollResult poll =
                    borderlessAccessAuthority.poll();
                if (poll.pending)
                {
                    if (!transition.applyObservation(
                            *action,
                            bafx::windows::
                                BackgroundCaptureActionObservation::Pending))
                    {
                        throw std::logic_error(
                            "Background capture transition rejected pending access");
                    }
                    execution.pending = true;
                    return BackgroundCaptureExecutionStatus::Pending;
                }
                if (!poll.result.has_value())
                {
                    throw std::logic_error(
                        "Borderless access request ended without a result");
                }
                appendBorderlessCaptureAccessCheck(
                    logPath,
                    execution.controlGeneration,
                    execution.actionIndex,
                    *poll.result);
                execution.borderlessAccessConfirmed =
                    bafx::windows::borderlessCaptureAccessAllowed(*poll.result);
                succeeded = execution.borderlessAccessConfirmed;
                if (!succeeded)
                {
                    execution.sensorFailure =
                        bafx::windows::borderlessCaptureAccessDiagnostic(
                            *poll.result);
                }
                break;
            }
            case bafx::windows::BackgroundCaptureActionKind::StopSensor:
                renderer.disableBackgroundCapture();
                succeeded = appendBackgroundCaptureStopDiagnostics(
                    logPath,
                    renderer,
                    "transaction").overallSucceeded;
                if (!succeeded)
                {
                    execution.sensorRestartAllowed = false;
                    execution.sensorFailure =
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
                const std::optional<LUID> requestedAdapter =
                    execution.targetIntent.target.sourceAdapterResolved
                        ? std::optional<LUID>(
                            execution.targetIntent.target.sourceAdapterLuid)
                        : std::nullopt;
                const DisplayOutputRetargetResult retarget =
                    retargetDisplayOutput(
                        window,
                        renderer,
                        DisplayOutputRetargetIntent{
                            execution.targetIntent.applyBounds
                                ? std::optional<RECT>(
                                    execution.targetIntent.target.bounds)
                                : std::nullopt,
                            requestedAdapter,
                            action->outputSize,
                            execution.targetIntent.outputPolicy});
                execution.outputAdapterRetargeted = retarget.adapter
                    != bafx::windows::OutputAdapterRetargetStatus::Unchanged;
                execution.outputAdapterWarpFallback = retarget.adapter
                    == bafx::windows::OutputAdapterRetargetStatus::
                        RecreatedWarpFallback;
                if (execution.outputAdapterWarpFallback)
                {
                    execution.sensorRestartAllowed = false;
                    execution.sensorFailure =
                        "Target display adapter fell back to WARP; WGC restart blocked";
                }
                execution.resizedOutputSize = action->outputSize;
                if (retarget.output
                    == bafx::windows::OutputResizeStatus::DeviceRecovered)
                {
                    observeDeviceRecovery(
                        execution,
                        retarget.deviceBeforeResize,
                        renderer,
                        logPath,
                        "Graphics.DeviceRecovery.ResizeSucceeded",
                        execution.sensorRestartAllowed);
                }
                else if (retarget.outputRenegotiation.has_value()
                    && retarget.outputRenegotiation->deviceRecovered)
                {
                    observeDeviceRecovery(
                        execution,
                        retarget.deviceBeforeOutputRenegotiation,
                        renderer,
                        logPath,
                        "Graphics.DeviceRecovery.OutputRenegotiationSucceeded",
                        execution.sensorRestartAllowed);
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
                    execution.recreatedFramePoolSize = action->captureSize;
                    break;
                case bafx::windows::BackgroundFramePoolRecreateStatus::Failed:
                    succeeded = false;
                    break;
                case bafx::windows::BackgroundFramePoolRecreateStatus::
                    DeviceRecovered:
                    succeeded = false;
                    observeDeviceRecovery(
                        execution,
                        previousDeviceInfo,
                        renderer,
                        logPath,
                        "Graphics.DeviceRecovery.FramePoolSucceeded",
                        execution.sensorRestartAllowed);
                    break;
                case bafx::windows::BackgroundFramePoolRecreateStatus::
                    DeviceRecoveryFailed:
                    throw std::runtime_error(
                        "Graphics device recovery failed during WGC frame pool recreate: "
                        + std::string(renderer.deviceRecoveryFailure()));
                }
                if (!succeeded && !renderer.backgroundCaptureFailure().empty())
                {
                    execution.sensorFailure = renderer.backgroundCaptureFailure();
                }
                break;
            }
            case bafx::windows::BackgroundCaptureActionKind::StartSensor:
                // Start is emitted only after WDA exclusion was confirmed in
                // this transaction, so stale affinity cannot enable capture.
                succeeded = execution.sensorRestartAllowed
                    && renderer.tryEnableBackgroundCapture(
                        execution.targetIntent.target.monitor,
                        true,
                        action->cursorExcluded,
                        action->allowSystemBorder,
                        execution.borderlessAccessConfirmed,
                        execution.targetIntent.target.captureRefreshRate);
                if (!execution.sensorRestartAllowed)
                {
                    if (execution.sensorFailure.empty())
                    {
                        execution.sensorFailure =
                            "WGC restart blocked after graphics adapter change or WARP recovery";
                    }
                }
                if (!succeeded && !renderer.backgroundCaptureFailure().empty())
                {
                    execution.sensorFailure = renderer.backgroundCaptureFailure();
                }
                break;
            }
        }
        catch (...)
        {
            appendBackgroundCaptureActionEnd(
                logPath,
                execution.actionIndex,
                action->kind,
                false,
                std::chrono::steady_clock::now() - execution.actionStartedAt);
            appendBackgroundCaptureResourceLedger(
                logPath,
                renderer,
                "action-failed");
            throw;
        }
        try
        {
            finishBackgroundCaptureAction(
                transition,
                execution,
                succeeded
                    ? bafx::windows::
                        BackgroundCaptureActionObservation::Succeeded
                    : bafx::windows::BackgroundCaptureActionObservation::Failed,
                logPath);
        }
        catch (...)
        {
            appendBackgroundCaptureResourceLedger(
                logPath,
                renderer,
                "transition-rejected");
            throw;
        }
    }

    std::string completion = "BackgroundCapture.Transaction.End;Actions=";
    completion += std::to_string(execution.executedActionCount);
    completion += ";ElapsedUs=";
    completion += std::to_string(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now()
            - execution.transactionStartedAt).count());
    bafx::windows::appendDiagnosticLog(logPath, completion);
    // Keep cumulative WGC ownership evidence beside every transaction.  A
    // failed stop/recreate can otherwise look successful after the sensor
    // pointer is released while an old WinRT resource is still live.
    appendBackgroundCaptureResourceLedger(logPath, renderer, "transaction");
    execution.transactionActive = false;
    execution.pending = false;
    execution.activeAction.reset();
    return BackgroundCaptureExecutionStatus::Completed;
}

void appendDisplayTopologyObserved(
    const std::filesystem::path& logPath,
    const std::uint64_t controlGeneration,
    const bool transactionActive,
    const DisplayTarget& applied,
    const std::uint32_t appliedDpi,
    const DisplayTarget& observed,
    const std::uint32_t windowEffectiveDpi) noexcept
{
    try
    {
        const std::string generation = std::to_string(controlGeneration);
        const std::string transaction = transactionActive ? "true" : "false";
        const std::string appliedMonitor = formatDisplayTargetMonitor(applied);
        const std::string appliedDevice = displayTargetDeviceUtf8(applied);
        const std::string appliedBounds = formatDisplayTargetBounds(applied);
        const std::string observedMonitor = formatDisplayTargetMonitor(observed);
        const std::string observedDevice = displayTargetDeviceUtf8(observed);
        const std::string observedBounds = formatDisplayTargetBounds(observed);
        const std::string appliedDpiText = std::to_string(appliedDpi);
        const std::string windowDpiText = std::to_string(windowEffectiveDpi);
        const std::string changed = sameDisplayTarget(applied, observed)
            && sameDisplaySourceIdentity(applied, observed)
            ? "false"
            : "true";
        const std::string dpiChanged = appliedDpi == windowEffectiveDpi
            ? "false"
            : "true";
        const std::array fields{
            bafx::windows::DiagnosticField{"Control.Generation", generation},
            bafx::windows::DiagnosticField{"Transaction.Active", transaction},
            bafx::windows::DiagnosticField{
                "Display.Applied.Monitor",
                appliedMonitor},
            bafx::windows::DiagnosticField{
                "Display.Applied.Device",
                appliedDevice},
            bafx::windows::DiagnosticField{
                "Display.Applied.Bounds",
                appliedBounds},
            bafx::windows::DiagnosticField{
                "Display.Applied.Dpi",
                appliedDpiText},
            bafx::windows::DiagnosticField{
                "Display.Observed.Monitor",
                observedMonitor},
            bafx::windows::DiagnosticField{
                "Display.Observed.Device",
                observedDevice},
            bafx::windows::DiagnosticField{
                "Display.Observed.Bounds",
                observedBounds},
            bafx::windows::DiagnosticField{"Display.Changed", changed},
            bafx::windows::DiagnosticField{
                "Window.EffectiveDpi",
                windowDpiText},
            bafx::windows::DiagnosticField{
                "Window.DpiChanged",
                dpiChanged}};
        bafx::windows::appendDiagnosticEvent(
            logPath,
            "Display.Topology.Observed",
            fields);
    }
    catch (...)
    {
        bafx::windows::appendDiagnosticLog(
            logPath,
            "Display.Topology.Observed=diagnostic-unavailable");
    }
}

void appendDisplayTopologyApplied(
    const std::filesystem::path& logPath,
    const std::uint64_t controlGeneration,
    const DisplayTarget& previous,
    const DisplayTarget& applied,
    const std::uint32_t dpi) noexcept
{
    try
    {
        const std::string generation = std::to_string(controlGeneration);
        const std::string previousMonitor = formatDisplayTargetMonitor(previous);
        const std::string previousDevice = displayTargetDeviceUtf8(previous);
        const std::string previousBounds = formatDisplayTargetBounds(previous);
        const std::string appliedMonitor = formatDisplayTargetMonitor(applied);
        const std::string appliedDevice = displayTargetDeviceUtf8(applied);
        const std::string appliedBounds = formatDisplayTargetBounds(applied);
        const std::string dpiText = std::to_string(dpi);
        const std::array fields{
            bafx::windows::DiagnosticField{"Control.Generation", generation},
            bafx::windows::DiagnosticField{
                "Display.Previous.Monitor",
                previousMonitor},
            bafx::windows::DiagnosticField{
                "Display.Previous.Device",
                previousDevice},
            bafx::windows::DiagnosticField{
                "Display.Previous.Bounds",
                previousBounds},
            bafx::windows::DiagnosticField{
                "Display.Applied.Monitor",
                appliedMonitor},
            bafx::windows::DiagnosticField{
                "Display.Applied.Device",
                appliedDevice},
            bafx::windows::DiagnosticField{
                "Display.Applied.Bounds",
                appliedBounds},
            bafx::windows::DiagnosticField{"Display.Applied.Dpi", dpiText}};
        bafx::windows::appendDiagnosticEvent(
            logPath,
            "Display.Topology.Applied",
            fields);
    }
    catch (...)
    {
        bafx::windows::appendDiagnosticLog(
            logPath,
            "Display.Topology.Applied=diagnostic-unavailable");
    }
}

BackgroundCaptureExecutionStatus cancelBackgroundCaptureTransition(
    bafx::windows::BackgroundCaptureTransition& transition,
    bafx::windows::OverlayWindow& window,
    bafx::windows::CompositionRenderer& renderer,
    bafx::windows::BorderlessCaptureAccessAuthority& borderlessAccessAuthority,
    BackgroundCaptureExecutionResult& execution,
    const BackgroundCaptureCancelResizePolicy resizePolicy,
    const std::string_view reason,
    const std::filesystem::path& logPath)
{
    if (!execution.transactionActive)
    {
        return BackgroundCaptureExecutionStatus::Completed;
    }
    if (!execution.pending
        || !execution.activeAction.has_value()
        || execution.activeAction->kind
            != bafx::windows::BackgroundCaptureActionKind::
                RequestBorderlessAccess)
    {
        throw std::logic_error(
            "Only a pending borderless access action can be canceled");
    }

    appendBackgroundCaptureCancellation(
        logPath,
        execution,
        resizePolicy,
        reason);
    // The system request belongs to the process authority. This transaction
    // only withdraws its observation so another display can keep waiting.
    execution.sensorFailure = "Borderless access request canceled; reason=";
    execution.sensorFailure += reason;
    execution.borderlessAccessConfirmed = false;
    finishBackgroundCaptureAction(
        transition,
        execution,
        resizePolicy == BackgroundCaptureCancelResizePolicy::Discard
            ? bafx::windows::BackgroundCaptureActionObservation::
                CanceledSupersededIntent
            : bafx::windows::BackgroundCaptureActionObservation::Canceled,
        logPath);

    return executeBackgroundCaptureTransition(
        transition,
        window,
        renderer,
        execution.targetIntent,
        execution.controlGeneration,
        borderlessAccessAuthority,
        execution,
        logPath);
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

void appendCaptureExclusionHealthFailure(
    const std::filesystem::path& logPath,
    const std::uint64_t controlGeneration,
    const bool transactionPending,
    const bafx::windows::CaptureExclusionQueryStatus& status) noexcept
{
    try
    {
        const std::array values{
            std::to_string(controlGeneration),
            win32Hex(status.expectedAffinity),
            win32Hex(status.observedAffinity),
            win32Hex(status.queryError)};
        const std::array fields{
            bafx::windows::DiagnosticField{"Control.Generation", values[0]},
            bafx::windows::DiagnosticField{
                "Transaction.Pending",
                transactionPending ? "true" : "false"},
            bafx::windows::DiagnosticField{
                "Capture.Exclusion.Expected",
                values[1]},
            bafx::windows::DiagnosticField{
                "Capture.Exclusion.Observed",
                values[2]},
            bafx::windows::DiagnosticField{
                "Capture.Exclusion.Query",
                status.querySucceeded ? "succeeded" : "failed"},
            bafx::windows::DiagnosticField{
                "Capture.Exclusion.QueryError",
                values[3]},
            bafx::windows::DiagnosticField{
                "Recovery",
                "stop-wgc-then-fallback-fx-only"}};
        bafx::windows::appendDiagnosticEvent(
            logPath,
            "WGC.CaptureExclusion.HealthFailed",
            fields,
            bafx::windows::DiagnosticLevel::Error);
    }
    catch (...)
    {
        // A logging allocation must not delay the required WGC stop.
    }
}

}
