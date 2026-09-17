#include "coordinator_output_runtime.hpp"
#include "background_capture_runtime.hpp"
#include "display_output_diagnostics.hpp"
#include "display_policy.hpp"
#include "host_control.hpp"

#include <array>
#include <stdexcept>
#include <utility>

namespace bafx::desktop
{
[[nodiscard]] std::optional<bafx::windows::OutputRenegotiationResult>
tryRenegotiateOutput(
    const std::filesystem::path& logPath,
    bafx::desktop::DisplaySession& session,
    const bafx::windows::CompositionOutputPolicy policy,
    const std::string_view reason) noexcept
{
    try
    {
        const bafx::windows::OutputRenegotiationResult result =
            session.renderer().renegotiateOutput(policy);
        bafx::desktop::appendOutputRenegotiation(logPath, session, reason, result);
        return result;
    }
    catch (const std::exception& error)
    {
        bafx::desktop::appendOutputRenegotiationFailure(
            logPath,
            session,
            policy,
            reason,
            error.what());
        return std::nullopt;
    }
    catch (...)
    {
        bafx::desktop::appendOutputRenegotiationFailure(
            logPath,
            session,
            policy,
            reason,
            "unknown exception");
        return std::nullopt;
    }
}

CoordinatorOutputRuntime::CoordinatorOutputRuntime(CoordinatorOutputContext context)
    : context_(std::move(context))
{
    const auto& session = context_.displaySession;
    const auto policy = resolveDisplayOutputPolicy(
        session.requestedOutputPreference(), session.colorCapabilities());
    if (session.renderer().outputPolicy() != policy
        || !bafx::windows::compositionOutputSatisfiesPolicy(
            session.renderer().outputState(), policy))
    {
        pending_ = PendingOutputRenegotiation{policy, "initial-output-fallback"};
    }
}

bool CoordinatorOutputRuntime::hasPending() const noexcept
{
    return pending_.has_value();
}

bool CoordinatorOutputRuntime::readyForAttempt(const bool maintenanceDue) const noexcept
{
    return pending_.has_value() && (!pending_->retryPending || maintenanceDue);
}

void CoordinatorOutputRuntime::clearPending() noexcept
{
    pending_.reset();
}

void CoordinatorOutputRuntime::servicePending(const bool backgroundCaptureRequested)
{
    if (!pending_.has_value())
    {
        return;
    }
    // Keep the current budget when servicing a queued attempt. Only an explicit
    // preference or recovery edge may introduce a fresh request.
    const auto pending = *pending_;
    if (attemptOutput(pending.policy, pending.reason, backgroundCaptureRequested))
    {
        pending_.reset();
    }
    else
    {
        retainFailed(pending);
    }
}

bool CoordinatorOutputRuntime::renegotiate(
    const bafx::windows::CompositionOutputPolicy policy,
    const std::string_view reason,
    const bool backgroundCaptureRequested)
{
    const bool applied = attemptOutput(policy, reason, backgroundCaptureRequested);
    if (!applied)
    {
        retainFailed(PendingOutputRenegotiation{policy, std::string(reason)});
    }
    return applied;
}

bool CoordinatorOutputRuntime::attemptOutput(
    const bafx::windows::CompositionOutputPolicy policy,
    const std::string_view reason,
    const bool backgroundCaptureRequested)
{
    auto& displaySession = context_.displaySession;
    auto& control = context_.control;
    auto& report = context_.report;
    auto& backgroundRetry = context_.backgroundRetry;
    auto& coordinatorCaptureSizeTracker = context_.coordinatorCaptureSizeTracker;
    auto& backgroundObservation = context_.backgroundObservation;
    auto& backgroundCaptureEnabled = context_.backgroundCaptureEnabled;
    auto& deviceRecoveryConsumed = context_.deviceRecoveryConsumed;
    auto& logPath = context_.logPath;
    auto& updateDisplayRuntimeSummary = context_.updateDisplayRuntimeSummary;
    auto& renderer = context_.displaySession.renderer();
    const bool backgroundCaptureWasActive =
        renderer.backgroundCaptureActive();
    const bool backgroundCaptureRestartRequired =
        backgroundCaptureRequested && backgroundCaptureEnabled;
    if (backgroundCaptureRestartRequired)
    {
        backgroundRetry.requireAvailable("WGC reconciliation token exhausted before output renegotiation");
    }

    const bafx::windows::GraphicsDeviceInfo previousDeviceInfo =
        renderer.deviceInfo();
    coordinatorCaptureSizeTracker.reset();
    // WGC shares this D3D resource domain. Retire it before replacing the
    // final swap chain so no callback can retain an old-domain texture.
    renderer.disableBackgroundCapture();
    const bafx::windows::WgcBackgroundStopDiagnostics stopDiagnostics =
        bafx::desktop::appendBackgroundCaptureStopDiagnostics(
            logPath,
            renderer,
            "output-renegotiation");
    backgroundCaptureEnabled = false;
    control.setBackgroundCaptureActive(false);
    backgroundObservation.reset();
    if (!stopDiagnostics.overallSucceeded)
    {
        const std::string monitor =
            bafx::desktop::formatDisplayTargetMonitor(
                displaySession.target());
        const std::array fields{
            bafx::windows::DiagnosticField{"Reason", reason},
            bafx::windows::DiagnosticField{"Monitor", monitor},
            bafx::windows::DiagnosticField{
                "RequestedPreference",
                bafx::desktop::outputPreferenceName(policy.preference)},
            bafx::windows::DiagnosticField{
                "WgcWasActive",
                backgroundCaptureWasActive ? "true" : "false"},
            bafx::windows::DiagnosticField{
                "WgcReconcile",
                backgroundCaptureRestartRequired
                    ? "blocked-stop-failed"
                    : "not-required"}};
        bafx::windows::appendDiagnosticEvent(
            logPath,
            "Display.Output.RenegotiationBlocked",
            fields,
            bafx::windows::DiagnosticLevel::Error);
        return false;
    }
    if (backgroundCaptureRestartRequired)
    {
        backgroundRetry.request();
    }

    const bool recoveryBudgetWasConsumed =
        renderer.deviceRecoveryBudgetConsumed();
    const auto result = tryRenegotiateOutput(
        logPath,
        displaySession,
        policy,
        reason);
    if (!result.has_value())
    {
        const bool recoveryBudgetConsumedByAttempt =
            !recoveryBudgetWasConsumed
            && renderer.deviceRecoveryBudgetConsumed();
        if (recoveryBudgetConsumedByAttempt)
        {
            deviceRecoveryConsumed = true;
            bafx::desktop::appendBackgroundCaptureStopDiagnostics(
                logPath,
                renderer,
                "output-renegotiation-device-recovery-failed");
            appendDeviceRemovedNotificationStatus(
                logPath,
                renderer,
                "output-renegotiation-device-recovery-failed");
            report.setDeviceInfo(renderer.deviceInfo());
            updateDisplayRuntimeSummary();
            bafx::windows::appendDiagnosticLog(logPath, report);

            const std::string recoveryFailure(
                renderer.deviceRecoveryFailure());
            if (!recoveryFailure.empty())
            {
                throw std::runtime_error(
                    "Output renegotiation device recovery failed: "
                    + recoveryFailure);
            }

            const std::array fields{
                bafx::windows::DiagnosticField{"Reason", reason},
                bafx::windows::DiagnosticField{
                    "Outcome",
                    "device-recovered-output-not-applied"}};
            bafx::windows::appendDiagnosticEvent(
                logPath,
                "Graphics.DeviceRecovery.OutputRenegotiationIncomplete",
                fields,
                bafx::windows::DiagnosticLevel::Error);
        }
        return false;
    }

    report.setDeviceInfo(renderer.deviceInfo());
    updateDisplayRuntimeSummary();
    const bool outputPolicySatisfied =
        renderer.outputPolicy() == policy
        && bafx::windows::compositionOutputSatisfiesPolicy(
            renderer.outputState(),
            policy);
    if (!result->deviceRecovered)
    {
        return outputPolicySatisfied;
    }

    // The explicit stop above already scheduled reconciliation. Recovery
    // only contributes adapter-domain evidence here.
    deviceRecoveryConsumed = true;
    bafx::desktop::appendBackgroundCaptureStopDiagnostics(
        logPath,
        renderer,
        "output-renegotiation-device-recovery");
    appendDeviceRemovedNotificationStatus(
        logPath,
        renderer,
        "output-renegotiation-device-recovery");
    const bool adapterChanged =
        previousDeviceInfo.adapterLuid.LowPart
            != renderer.deviceInfo().adapterLuid.LowPart
        || previousDeviceInfo.adapterLuid.HighPart
            != renderer.deviceInfo().adapterLuid.HighPart;
    const std::string retryTokenText = std::to_string(
        backgroundRetry.token());
    const std::array fields{
        bafx::windows::DiagnosticField{
            "Reason",
            reason},
        bafx::windows::DiagnosticField{
            "Adapter",
            adapterChanged ? "changed" : "same"},
        bafx::windows::DiagnosticField{
            "WgcWasActive",
            backgroundCaptureWasActive ? "true" : "false"},
        bafx::windows::DiagnosticField{
            "WgcRestart",
            backgroundCaptureRestartRequired
                ? "scheduled"
                : "not-required"},
        bafx::windows::DiagnosticField{
            "ReconcileToken",
            retryTokenText}};
    bafx::windows::appendDiagnosticEvent(
        logPath,
        "Graphics.DeviceRecovery.OutputRenegotiationSucceeded",
        fields,
        bafx::windows::DiagnosticLevel::Warning);
    return outputPolicySatisfied;
}

void CoordinatorOutputRuntime::retainFailed(PendingOutputRenegotiation pending)
{
    auto& displaySession = context_.displaySession;
    auto& report = context_.report;
    auto& logPath = context_.logPath;
    auto& updateDisplayRuntimeSummary = context_.updateDisplayRuntimeSummary;
    auto& renderer = context_.displaySession.renderer();
    if (!pending.budget.retryAfterFailure())
    {
        const bafx::desktop::DisplayOutputExhaustionDisposition
            disposition =
                bafx::desktop::
                    resolveDisplayOutputExhaustionDisposition(
                        renderer.outputState());
        bafx::desktop::appendOutputRenegotiationExhausted(
            logPath,
            displaySession,
            pending.policy,
            pending.reason,
            disposition);
        pending_.reset();
        if (disposition
            == bafx::desktop::
                DisplayOutputExhaustionDisposition::FailClosed)
        {
            // The Host shell owns the coordinator surface. Hiding it
            // before terminating guarantees an old scRGB contract
            // cannot remain visible after the user's SDR request.
            displaySession.markRenderFaulted();
            report.setDeviceInfo(renderer.deviceInfo());
            updateDisplayRuntimeSummary();
            bafx::windows::appendDiagnosticLog(logPath, report);
            throw std::runtime_error(
                "Coordinator output renegotiation exhausted while a non-SDR transport remained active");
        }
        return;
    }

    pending.retryPending = true;
    pending_ = std::move(pending);
    bafx::desktop::appendOutputRenegotiationRetryScheduled(
        logPath,
        displaySession,
        pending_->policy,
        pending_->reason,
        pending_->budget.remaining(),
        "display-maintenance");
}

void CoordinatorOutputRuntime::queueRecoveryIfNeeded(const std::string_view reason)
{
    auto& displaySession = context_.displaySession;
    auto& logPath = context_.logPath;
    auto& renderer = context_.displaySession.renderer();
    const bafx::windows::CompositionOutputPolicy policy =
        bafx::desktop::resolveDisplayOutputPolicy(
            displaySession.requestedOutputPreference(),
            displaySession.colorCapabilities());
    const bool outputPolicySatisfied =
        renderer.outputPolicy() == policy
        && bafx::windows::compositionOutputSatisfiesPolicy(
            renderer.outputState(),
            policy);
    const bool duplicatePending =
        pending_.has_value()
        && pending_->policy == policy;
    if (outputPolicySatisfied || duplicatePending)
    {
        return;
    }

    // A device/resource-domain recovery is a new finite recovery edge.
    // Ordinary WGC transactions never call this helper, so an exhausted
    // output budget cannot silently become an unbounded retry loop.
    pending_ =
        PendingOutputRenegotiation{
            policy,
            std::string(reason)};
    const std::string attempts = std::to_string(
        pending_->budget.remaining());
    const std::array fields{
        bafx::windows::DiagnosticField{"Reason", reason},
        bafx::windows::DiagnosticField{
            "RequestedPreference",
            bafx::desktop::outputPreferenceName(policy.preference)},
        bafx::windows::DiagnosticField{
            "ActualTransfer",
            bafx::desktop::outputTransferName(renderer.outputState().transfer)},
        bafx::windows::DiagnosticField{
            "Fallback",
            bafx::desktop::outputFallbackName(renderer.outputState().fallback)},
        bafx::windows::DiagnosticField{"Attempts", attempts}};
    bafx::windows::appendDiagnosticEvent(
        logPath,
        "Display.Output.RecoveryReconciliationQueued",
        fields,
        bafx::windows::DiagnosticLevel::Warning);
}

void CoordinatorOutputRuntime::refreshColorState(
    const std::string_view reason,
    const std::uint64_t generation,
    const bool outputRebuiltForCurrentTarget,
    const std::optional<bafx::windows::DisplayColorCapabilities>& fallbackCapabilities,
    const DisplayTarget& appliedDisplayTarget,
    const DisplaySessionColorRefreshRequest request)
{
    auto& displaySession = context_.displaySession;
    auto& report = context_.report;
    auto& logPath = context_.logPath;
    auto& updateDisplayRuntimeSummary = context_.updateDisplayRuntimeSummary;
    auto& renderer = context_.displaySession.renderer();
    const std::optional<bafx::windows::DisplayColorCapabilities>
        previousCapabilities = displaySession.colorCapabilities();
    const std::string previousMode =
        previousCapabilities.has_value()
        ? std::string(bafx::windows::displayColorModeName(
            previousCapabilities->activeColorMode))
        : "unknown";
    const bafx::desktop::DisplaySessionColorRefreshStatus refreshStatus =
        displaySession.refreshColorCapabilities(
            fallbackCapabilities,
            request);
    if (displaySession.colorCapabilities().has_value())
    {
        report.setPrimaryDisplayColorCapabilities(
            *displaySession.colorCapabilities());
    }
    else
    {
        report.clearPrimaryDisplayColorCapabilities();
    }
    report.setPrimaryDisplayColorMonitorResult(
        displaySession.colorMonitorResult());

    const std::string currentMode =
        displaySession.colorCapabilities().has_value()
        ? std::string(bafx::windows::displayColorModeName(
            displaySession.colorCapabilities()->activeColorMode))
        : "unknown";
    const std::string monitor =
        bafx::desktop::formatDisplayTargetMonitor(
            appliedDisplayTarget);
    const std::string generationText = std::to_string(generation);
    const std::string retriesRemaining = std::to_string(
        displaySession.colorRefreshRetriesRemaining());
    const bafx::windows::CompositionOutputPreference
        requestedPreference =
            displaySession.requestedOutputPreference();
    const bafx::windows::CompositionOutputPolicy previousPolicy =
        bafx::desktop::resolveDisplayOutputPolicy(
            requestedPreference,
            previousCapabilities);
    const bafx::windows::CompositionOutputPolicy currentPolicy =
        bafx::desktop::resolveDisplayOutputPolicy(
            requestedPreference,
            displaySession.colorCapabilities());
    const bool outputPolicyMismatch =
        renderer.outputPolicy() != currentPolicy
        || !bafx::windows::compositionOutputSatisfiesPolicy(
            renderer.outputState(),
            currentPolicy);
    const bool colorContractChanged =
        bafx::desktop::displayOutputContractChanged(
            previousPolicy.preference,
            currentPolicy.preference,
            previousCapabilities,
            displaySession.colorCapabilities());
    // A target migration already recreated the swap chain after the
    // HWND moved. Reconcile only a pre/post query disagreement there;
    // ordinary color events must also rebuild same-transfer metadata.
    const bool outputContractChanged = outputPolicyMismatch
        || (!outputRebuiltForCurrentTarget
            && colorContractChanged);
    if (outputContractChanged)
    {
        const bool duplicatePendingContract =
            pending_.has_value()
            && pending_->policy
                == currentPolicy;
        if (!duplicatePendingContract)
        {
            // A duplicate OS notification must not reset the finite
            // retry budget of the same desired transport contract.
            pending_ =
                PendingOutputRenegotiation{
                    currentPolicy,
                    std::string(reason)};
        }
    }
    else if (outputRebuiltForCurrentTarget)
    {
        // A completed display retarget already rebuilt or reaffirmed
        // the output for the new monitor; discard stale notifications.
        pending_.reset();
    }
    const std::array fields{
        bafx::windows::DiagnosticField{"Reason", reason},
        bafx::windows::DiagnosticField{"Monitor", monitor},
        bafx::windows::DiagnosticField{"PreviousMode", previousMode},
        bafx::windows::DiagnosticField{"CurrentMode", currentMode},
        bafx::windows::DiagnosticField{
            "Query",
            colorRefreshStatusName(refreshStatus)},
        bafx::windows::DiagnosticField{
            "Generation",
            generationText},
        bafx::windows::DiagnosticField{
            "RetryBudgetRemaining",
            retriesRemaining},
        bafx::windows::DiagnosticField{
            "RequestedPreference",
            bafx::desktop::outputPreferenceName(requestedPreference)},
        bafx::windows::DiagnosticField{
            "ResolvedPreference",
            bafx::desktop::outputPreferenceName(currentPolicy.preference)},
        bafx::windows::DiagnosticField{
            "OutputContract",
            outputContractChanged ? "changed" : "unchanged"},
        bafx::windows::DiagnosticField{
            "OutputRenegotiation",
            outputContractChanged ? "queued" : "not-needed"}};
    bafx::windows::appendDiagnosticEvent(
        logPath,
        "Display.ColorState.Refreshed",
        fields);
    updateDisplayRuntimeSummary();
    bafx::windows::appendDiagnosticLog(logPath, report);
}

}
