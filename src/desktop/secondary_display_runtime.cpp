#include "secondary_display_runtime.hpp"
#include "display_output_diagnostics.hpp"
#include "frame_visual_config.hpp"
#include "performance_samples.hpp"

#include "bafx/windows/error.hpp"
#include "bafx/windows/runtime_diagnostics.hpp"

#include <algorithm>
#include <array>
#include <exception>
#include <string>

namespace bafx::desktop
{
namespace
{

[[nodiscard]] bool secondaryDeviceRemovalPending(
    const bafx::desktop::DisplaySession& session) noexcept
{
    const HANDLE deviceRemoved =
        session.renderer().deviceRemovedWaitableObject();
    if (deviceRemoved == nullptr)
    {
        return false;
    }

    // RegisterDeviceRemovedEvent uses a manual-reset event. Polling it here
    // does not consume the recovery signal that the frame-pacing owner needs.
    return WaitForSingleObject(deviceRemoved, 0U) == WAIT_OBJECT_0;
}

[[nodiscard]] std::string_view secondaryBackgroundRecoveryStatusName(
    const bafx::desktop::DisplaySessionBackgroundRecoveryStatus status) noexcept
{
    switch (status)
    {
    case bafx::desktop::DisplaySessionBackgroundRecoveryStatus::NotRequired:
        return "not-required";
    case bafx::desktop::DisplaySessionBackgroundRecoveryStatus::Queued:
        return "queued";
    case bafx::desktop::DisplaySessionBackgroundRecoveryStatus::Blocked:
        return "blocked";
    }
    return "unknown";
}

}

void appendSecondaryRenderFailure(
    const std::filesystem::path& logPath,
    const bafx::desktop::DisplaySession& session,
    const std::string_view operation,
    const std::string_view message) noexcept
{
    try
    {
        const std::string device =
            bafx::desktop::displayTargetDeviceUtf8(session.target());
        const std::string monitor =
            bafx::desktop::formatDisplayTargetMonitor(session.target());
        const std::string bounds =
            bafx::desktop::formatDisplayTargetBounds(session.target());
        const std::array fields{
            bafx::windows::DiagnosticField{"Operation", operation},
            bafx::windows::DiagnosticField{"Device", device},
            bafx::windows::DiagnosticField{"Monitor", monitor},
            bafx::windows::DiagnosticField{"Bounds", bounds},
            bafx::windows::DiagnosticField{"Message", message}};
        bafx::windows::appendDiagnosticEvent(
            logPath,
            "Display.Session.RenderFailed",
            fields,
            bafx::windows::DiagnosticLevel::Error);
    }
    catch (...)
    {
        bafx::windows::appendDiagnosticLog(
            logPath,
            "Secondary display render failure could not be formatted");
    }
}

void appendSecondaryBackgroundCaptureFailure(
    const std::filesystem::path& logPath,
    const bafx::desktop::DisplaySession& session,
    const std::string_view operation,
    const std::string_view message) noexcept
{
    try
    {
        const std::string device =
            bafx::desktop::displayTargetDeviceUtf8(session.target());
        const std::string monitor =
            bafx::desktop::formatDisplayTargetMonitor(session.target());
        const std::array fields{
            bafx::windows::DiagnosticField{"Operation", operation},
            bafx::windows::DiagnosticField{"Device", device},
            bafx::windows::DiagnosticField{"Monitor", monitor},
            bafx::windows::DiagnosticField{"Message", message},
            bafx::windows::DiagnosticField{"Fallback", "fx-only"}};
        bafx::windows::appendDiagnosticEvent(
            logPath,
            "Display.Session.BackgroundCaptureFailed",
            fields,
            bafx::windows::DiagnosticLevel::Error);
    }
    catch (...)
    {
        bafx::windows::appendDiagnosticLog(
            logPath,
            "Secondary background capture failure could not be formatted");
    }
}

void applySecondaryBackgroundCaptureRequest(
    bafx::desktop::DisplaySessionManager& sessions,
    bafx::desktop::DisplaySession& coordinator,
    const bafx::windows::BackgroundCaptureRequest& request,
    const std::uint64_t controlGeneration,
    const std::filesystem::path& logPath,
    const bool powerUnavailable) noexcept
{
    for (const auto& ownedSession : sessions.sessions())
    {
        bafx::desktop::DisplaySession& session = *ownedSession;
        if (&session == &coordinator
            || (session.renderFaulted()
                && !session.outputContractFaulted()))
        {
            continue;
        }

        try
        {
            if (session.secondaryBackgroundCaptureInitialized())
            {
                session.updateSecondaryBackgroundCaptureRequest(
                    request,
                    controlGeneration);
            }
            else
            {
                session.initializeSecondaryBackgroundCapture(
                    request,
                    controlGeneration,
                    logPath,
                    powerUnavailable);
            }
        }
        catch (const std::exception& error)
        {
            // WGC is optional per surface. Retire only this transaction and
            // preserve every other display plus this surface's FX-only path.
            session.shutdownSecondaryBackgroundCapture();
            appendSecondaryBackgroundCaptureFailure(
                logPath,
                session,
                "apply-request",
                error.what());
        }
        catch (...)
        {
            session.shutdownSecondaryBackgroundCapture();
            appendSecondaryBackgroundCaptureFailure(
                logPath,
                session,
                "apply-request",
                "unknown exception");
        }
    }
}

[[nodiscard]] bool handleSecondaryBorderlessAccessLosses(
    bafx::desktop::DisplaySessionManager& sessions,
    bafx::desktop::DisplaySession& coordinator,
    const bafx::core::MonotonicTime now,
    const std::filesystem::path& logPath) noexcept
{
    bool renderInvalidated = false;
    for (const auto& ownedSession : sessions.sessions())
    {
        bafx::desktop::DisplaySession& session = *ownedSession;
        if (&session == &coordinator
            || session.renderFaulted()
            || !session.secondaryBackgroundCaptureInitialized())
        {
            continue;
        }

        try
        {
            const bafx::desktop::DisplaySessionBackgroundCaptureServiceResult
                result = session.handleSecondaryBorderlessAccessLost(now);
            renderInvalidated = result.renderInvalidated || renderInvalidated;
            appendSecondaryBackgroundCaptureServiceResult(
                logPath,
                session,
                result);
        }
        catch (const std::exception& error)
        {
            session.shutdownSecondaryBackgroundCapture();
            appendSecondaryBackgroundCaptureFailure(
                logPath,
                session,
                "borderless-access-lost",
                error.what());
        }
        catch (...)
        {
            session.shutdownSecondaryBackgroundCapture();
            appendSecondaryBackgroundCaptureFailure(
                logPath,
                session,
                "borderless-access-lost",
                "unknown exception");
        }
    }
    return renderInvalidated;
}

[[nodiscard]] bool retrySecondaryBorderlessAccess(
    bafx::desktop::DisplaySessionManager& sessions,
    bafx::desktop::DisplaySession& coordinator,
    const std::uint64_t controlGeneration,
    const bafx::core::MonotonicTime now,
    const std::filesystem::path& logPath) noexcept
{
    bool renderInvalidated = false;
    for (const auto& ownedSession : sessions.sessions())
    {
        bafx::desktop::DisplaySession& session = *ownedSession;
        if (&session == &coordinator
            || session.renderFaulted()
            || !session.secondaryBackgroundCaptureInitialized())
        {
            continue;
        }

        try
        {
            if (!session.retrySecondaryBorderlessAccess(controlGeneration))
            {
                continue;
            }
            const bafx::desktop::DisplaySessionBackgroundCaptureServiceResult
                result = session.serviceSecondaryBackgroundCapture(now);
            renderInvalidated = true;
            appendSecondaryBackgroundCaptureServiceResult(
                logPath,
                session,
                result);
        }
        catch (const std::exception& error)
        {
            session.shutdownSecondaryBackgroundCapture();
            appendSecondaryBackgroundCaptureFailure(
                logPath,
                session,
                "borderless-access-retry",
                error.what());
        }
        catch (...)
        {
            session.shutdownSecondaryBackgroundCapture();
            appendSecondaryBackgroundCaptureFailure(
                logPath,
                session,
                "borderless-access-retry",
                "unknown exception");
        }
    }
    return renderInvalidated;
}

void appendSecondaryBackgroundCaptureServiceResult(
    const std::filesystem::path& logPath,
    const bafx::desktop::DisplaySession& session,
    const bafx::desktop::DisplaySessionBackgroundCaptureServiceResult& result)
    noexcept
{
    if (result.outputRenegotiationDiscarded
        && result.outputRenegotiationTarget.has_value())
    {
        bafx::desktop::appendOutputRenegotiationDiscarded(
            logPath,
            session,
            *result.outputRenegotiationTarget,
            result.outputRenegotiationPolicy,
            result.outputRenegotiationReason);
    }
    if (result.outputRenegotiation.has_value())
    {
        bafx::desktop::appendOutputRenegotiation(
            logPath,
            session,
            result.outputRenegotiationReason,
            *result.outputRenegotiation);
    }
    if (!result.outputRenegotiationFailure.empty())
    {
        bafx::desktop::appendOutputRenegotiationFailure(
            logPath,
            session,
            result.outputRenegotiationPolicy,
            result.outputRenegotiationReason,
            result.outputRenegotiationFailure,
            result.deviceRecovered);
    }
    if (result.outputRenegotiationRetryPending)
    {
        bafx::desktop::appendOutputRenegotiationRetryScheduled(
            logPath,
            session,
            result.outputRenegotiationPolicy,
            result.outputRenegotiationReason,
            result.outputRenegotiationRetriesRemaining,
            "one-second-monotonic");
    }
    if (result.outputRenegotiationExhausted)
    {
        bafx::desktop::appendOutputRenegotiationExhausted(
            logPath,
            session,
            result.outputRenegotiationPolicy,
            result.outputRenegotiationReason,
            result.outputRenegotiationFailedClosed
                ? bafx::desktop::DisplayOutputExhaustionDisposition::FailClosed
                : bafx::desktop::DisplayOutputExhaustionDisposition::
                    AcceptConservativeFallback);
    }
}

[[nodiscard]] bool serviceSecondaryBackgroundCaptures(
    bafx::desktop::DisplaySessionManager& sessions,
    bafx::desktop::DisplaySession& coordinator,
    const bafx::core::MonotonicTime now,
    const std::filesystem::path& logPath) noexcept
{
    bool renderInvalidated = false;
    for (const auto& ownedSession : sessions.sessions())
    {
        bafx::desktop::DisplaySession& session = *ownedSession;
        if (&session == &coordinator)
        {
            continue;
        }
        if (session.renderFaulted()
            && !session.outputContractRecoveryActionable())
        {
            session.shutdownSecondaryBackgroundCapture();
            continue;
        }
        if (!session.secondaryBackgroundCaptureInitialized())
        {
            continue;
        }
        if (secondaryDeviceRemovalPending(session))
        {
            renderInvalidated = true;
            continue;
        }

        try
        {
            const bafx::desktop::DisplaySessionBackgroundCaptureServiceResult
                result = session.serviceSecondaryBackgroundCapture(now);
            renderInvalidated = result.renderInvalidated || renderInvalidated;
            appendSecondaryBackgroundCaptureServiceResult(
                logPath,
                session,
                result);
        }
        catch (const std::exception& error)
        {
            session.shutdownSecondaryBackgroundCapture();
            appendSecondaryBackgroundCaptureFailure(
                logPath,
                session,
                "service",
                error.what());
        }
        catch (...)
        {
            session.shutdownSecondaryBackgroundCapture();
            appendSecondaryBackgroundCaptureFailure(
                logPath,
                session,
                "service",
                "unknown exception");
        }
    }
    return renderInvalidated;
}

[[nodiscard]] bool maintainSecondaryBackgroundCaptures(
    bafx::desktop::DisplaySessionManager& sessions,
    bafx::desktop::DisplaySession& coordinator,
    const std::span<bafx::desktop::DisplaySession*> readySessions,
    const bafx::core::MonotonicTime now,
    const std::filesystem::path& logPath) noexcept
{
    bool renderInvalidated = false;
    for (const auto& ownedSession : sessions.sessions())
    {
        bafx::desktop::DisplaySession& session = *ownedSession;
        if (&session == &coordinator
            || (session.renderFaulted()
                && !session.outputContractRecoveryActionable())
            || !session.secondaryBackgroundCaptureInitialized()
            || std::find(
                readySessions.begin(),
                readySessions.end(),
                &session) != readySessions.end())
        {
            continue;
        }
        if (secondaryDeviceRemovalPending(session))
        {
            renderInvalidated = true;
            continue;
        }

        try
        {
            if (session.secondaryBackgroundCaptureActive())
            {
                // A WGC callback does not grant a swap-chain slot. Keep only
                // the newest owned sample so a faster capture source cannot
                // accumulate work behind a slower secondary Present cadence.
                const bafx::windows::BackgroundSensorMaintenanceDiagnostics
                    maintenance =
                        session.renderer().serviceBackgroundCapture(now);
                renderInvalidated =
                    (maintenance.wgc.accepted
                        && session.lastPresentedDrawableContent())
                    || renderInvalidated;
            }
            const bafx::desktop::DisplaySessionBackgroundCaptureServiceResult
                result = session.serviceSecondaryBackgroundCapture(now);
            renderInvalidated = result.renderInvalidated || renderInvalidated;
            appendSecondaryBackgroundCaptureServiceResult(
                logPath,
                session,
                result);
        }
        catch (const std::exception& error)
        {
            session.shutdownSecondaryBackgroundCapture();
            appendSecondaryBackgroundCaptureFailure(
                logPath,
                session,
                "maintenance",
                error.what());
        }
        catch (...)
        {
            session.shutdownSecondaryBackgroundCapture();
            appendSecondaryBackgroundCaptureFailure(
                logPath,
                session,
                "maintenance",
                "unknown exception");
        }
    }
    return renderInvalidated;
}

void appendSecondaryDeviceRecovery(
    const std::filesystem::path& logPath,
    const bafx::desktop::DisplaySession& session,
    const bafx::desktop::DisplaySessionDeviceRecoveryResult& recovery,
    const std::string_view eventName) noexcept
{
    try
    {
        const std::string monitor =
            bafx::desktop::formatDisplayTargetMonitor(session.target());
        const std::array fields{
            bafx::windows::DiagnosticField{"Monitor", monitor},
            bafx::windows::DiagnosticField{
                "Driver",
                session.renderer().deviceInfo().driverType
                        == bafx::windows::GraphicsDriverType::Hardware
                    ? "hardware"
                    : "warp"},
            bafx::windows::DiagnosticField{
                "Adapter",
                recovery.adapterChanged ? "changed" : "same"},
            bafx::windows::DiagnosticField{
                "WgcWasActive",
                recovery.backgroundWasActive ? "true" : "false"},
            bafx::windows::DiagnosticField{
                "WgcRestart",
                secondaryBackgroundRecoveryStatusName(recovery.background)}};
        bafx::windows::appendDiagnosticEvent(
            logPath,
            eventName,
            fields,
            bafx::windows::DiagnosticLevel::Warning);
    }
    catch (...)
    {
        bafx::windows::appendDiagnosticLog(
            logPath,
            "Secondary device recovery could not be formatted");
    }
}

[[nodiscard]] bool recoverSecondaryDisplaySession(
    const std::filesystem::path& logPath,
    bafx::desktop::DisplaySession& session,
    const std::string_view failureOperation,
    const std::string_view successEvent,
    const bool validateRemovalReason) noexcept
{
    if (validateRemovalReason)
    {
        const HRESULT removalReason =
            session.renderer().deviceRemovedReason();
        if (!bafx::windows::isDeviceLostResult(removalReason))
        {
            session.markRenderFaulted();
            try
            {
                appendSecondaryRenderFailure(
                    logPath,
                    session,
                    failureOperation,
                    "device removal notification produced unexpected HRESULT "
                        + formatHresult(removalReason));
            }
            catch (...)
            {
                // Preserve per-display isolation even if formatting the
                // unexpected driver result cannot allocate memory.
                appendSecondaryRenderFailure(
                    logPath,
                    session,
                    failureOperation,
                    "device removal notification produced unexpected HRESULT");
            }
            return false;
        }
    }

    const bafx::desktop::DisplaySessionDeviceRecoveryResult recovery =
        session.tryRecoverDevice();
    if (!recovery.recovered)
    {
        session.markRenderFaulted();
        appendSecondaryRenderFailure(
            logPath,
            session,
            failureOperation,
            session.renderer().deviceRecoveryFailure());
        return false;
    }

    session.clearRenderFault();
    appendSecondaryDeviceRecovery(
        logPath,
        session,
        recovery,
        successEvent);
    return true;
}

SecondaryRenderSummary renderSecondarySessions(
    bafx::desktop::DisplaySessionManager& sessions,
    bafx::desktop::DisplaySession& coordinator,
    const std::span<bafx::desktop::DisplaySession*> readySessions,
    const bafx::config::Config& config,
    const bafx::fx::SimulationTime renderTime,
    const bafx::core::MonotonicTime wallTime,
    const bool commitSimulationFrame,
    const bool requireCurrentBackground,
    const std::filesystem::path& logPath)
{
    SecondaryRenderSummary summary{};
    for (const auto& ownedSession : sessions.sessions())
    {
        bafx::desktop::DisplaySession& session = *ownedSession;
        if (&session == &coordinator
            || !session.effectsEnabled()
            || session.renderFaulted())
        {
            continue;
        }

        bafx::windows::CompositionRenderer& sessionRenderer =
            session.renderer();
        const HANDLE deviceRemoved =
            sessionRenderer.deviceRemovedWaitableObject();
        if (deviceRemoved != nullptr
            && WaitForSingleObject(deviceRemoved, 0U) == WAIT_OBJECT_0)
        {
            if (!recoverSecondaryDisplaySession(
                    logPath,
                    session,
                    "device-recovery",
                    "Display.Session.DeviceRecovered",
                    true))
            {
                ++summary.failed;
                continue;
            }
            ++summary.recovered;
            // The recovered swap chain owns a new latency handle. An
            // opportunity granted by the released handle cannot authorize a
            // Present on this resource domain.
            continue;
        }

        const bool frameReady = std::find(
            readySessions.begin(),
            readySessions.end(),
            &session) != readySessions.end();
        if (!frameReady)
        {
            ++summary.notReady;
            continue;
        }

        bafx::fx::FrameSnapshot snapshot = session.simulation().snapshot(
            toViewport(session.window().size()),
            renderTime);
        applyVisualConfig(snapshot, config);
        try
        {
            const bafx::windows::CompositionFrameDiagnostics diagnostics =
                sessionRenderer.renderFrame(
                snapshot,
                wallTime,
                requireCurrentBackground);
            session.recordActiveFxRoiFrame(
                bafx::desktop::framePerformanceSample(diagnostics, 0U, false),
                diagnostics.frameId,
                wallTime);
            session.recordPresentedFrame(
                snapshot.hasDrawableContent(),
                wallTime);
            if (commitSimulationFrame)
            {
                session.simulation().onFrameRendered(renderTime);
            }
            ++summary.rendered;
        }
        catch (const bafx::windows::HResultError& error)
        {
            if (bafx::windows::isDeviceLostResult(error.result()))
            {
                if (recoverSecondaryDisplaySession(
                        logPath,
                        session,
                        "render-device-recovery",
                        "Display.Session.RenderDeviceRecovered",
                        false))
                {
                    ++summary.recovered;
                    continue;
                }
                ++summary.failed;
                continue;
            }
            session.markRenderFaulted();
            ++summary.failed;
            appendSecondaryRenderFailure(
                logPath,
                session,
                "render",
                error.what());
        }
        catch (const std::exception& error)
        {
            session.markRenderFaulted();
            ++summary.failed;
            appendSecondaryRenderFailure(
                logPath,
                session,
                "render",
                error.what());
        }
    }
    return summary;
}

}
