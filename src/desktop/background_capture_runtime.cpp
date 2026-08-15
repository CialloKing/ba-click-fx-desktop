#include "background_capture_runtime.hpp"

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

[[nodiscard]] std::string win32Hex(const DWORD value)
{
    std::ostringstream stream;
    stream << "0x" << std::hex << std::uppercase << std::setw(8)
           << std::setfill('0') << value;
    return stream.str();
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
    execution.borderlessAccessRequest.reset();
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
        const std::array values{
            std::to_string(controlGeneration),
            std::to_string(actionIndex),
            errorStream.str(),
            std::to_string(result.elapsedMilliseconds)};
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
                    : "false"}};
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
                if (execution.borderlessAccessRequest == nullptr)
                {
                    execution.borderlessAccessRequest = std::make_unique<
                        bafx::windows::BorderlessCaptureAccessRequest>();
                    execution.borderlessAccessRequest->begin(
                        bafx::windows::queryCurrentPackageIdentity());
                }
                const bafx::windows::BorderlessCaptureAccessPollResult poll =
                    execution.borderlessAccessRequest->poll();
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
                execution.borderlessAccessRequest.reset();
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
                if (execution.targetIntent.applyBounds)
                {
                    window.setBounds(execution.targetIntent.target.bounds);
                    if (window.size().width != action->outputSize.width
                        || window.size().height != action->outputSize.height)
                    {
                        throw std::runtime_error(
                            "Overlay client size does not match the display target");
                    }
                }
                const bafx::windows::GraphicsDeviceInfo previousDeviceInfo =
                    renderer.deviceInfo();
                const bafx::windows::OutputResizeStatus resizeStatus =
                    renderer.resizeOutput(action->outputSize);
                execution.resizedOutputSize = action->outputSize;
                if (resizeStatus
                    == bafx::windows::OutputResizeStatus::DeviceRecovered)
                {
                    observeDeviceRecovery(
                        execution,
                        previousDeviceInfo,
                        renderer,
                        logPath,
                        "Graphics.DeviceRecovery.ResizeSucceeded",
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
                        execution.borderlessAccessConfirmed);
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
    execution.borderlessAccessRequest.reset();
    return BackgroundCaptureExecutionStatus::Completed;
}

BackgroundCaptureExecutionStatus cancelBackgroundCaptureTransition(
    bafx::windows::BackgroundCaptureTransition& transition,
    bafx::windows::OverlayWindow& window,
    bafx::windows::CompositionRenderer& renderer,
    BackgroundCaptureExecutionResult& execution,
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
                RequestBorderlessAccess
        || execution.borderlessAccessRequest == nullptr)
    {
        throw std::logic_error(
            "Only a pending borderless access action can be canceled");
    }

    appendBackgroundCaptureCancellation(logPath, execution, reason);
    execution.borderlessAccessRequest->cancel();
    const bafx::windows::BorderlessCaptureAccessPollResult poll =
        execution.borderlessAccessRequest->poll();
    if (!poll.result.has_value())
    {
        throw std::logic_error(
            "Canceled borderless access request did not produce a result");
    }
    appendBorderlessCaptureAccessCheck(
        logPath,
        execution.controlGeneration,
        execution.actionIndex,
        *poll.result);
    execution.sensorFailure = "Borderless access request canceled; reason=";
    execution.sensorFailure += reason;
    execution.borderlessAccessConfirmed = false;
    execution.borderlessAccessRequest.reset();
    finishBackgroundCaptureAction(
        transition,
        execution,
        bafx::windows::BackgroundCaptureActionObservation::Canceled,
        logPath);

    return executeBackgroundCaptureTransition(
        transition,
        window,
        renderer,
        execution.targetIntent,
        execution.controlGeneration,
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
