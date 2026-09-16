#include "display_runtime_summary.hpp"
#include "display_output_retarget.hpp"
#include "display_session_manager.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace bafx::desktop
{

[[nodiscard]] bafx::windows::DisplayRuntimeSummary collectDisplayRuntimeSummary(
    const DisplaySessionManager& displaySessions,
    const DisplaySession& displaySession,
    bafx::windows::DisplayTopologyStatus latestDisplayTopologyStatus,
    LONG latestDisplayTopologyError,
    bafx::core::MonotonicTime runtimeObservedAt)
{
    const auto& renderer = displaySession.renderer();
    const auto& capabilities = displaySession.colorCapabilities();
    const bool colorSnapshotComplete = capabilities.has_value()
        && bafx::windows::displayColorStateComplete(*capabilities);
    const bool hdrCapabilityObserved = capabilities.has_value()
        && (capabilities->advancedColorInfoV2
            || capabilities->advancedColorQueryResult == ERROR_SUCCESS
            || capabilities->advancedColorSupported
            || capabilities->highDynamicRangeSupported
            || capabilities->activeColorMode
                == bafx::windows::DisplayColorMode::Hdr);
    const bool hdrActive = colorSnapshotComplete
        && capabilities->activeColorMode
            == bafx::windows::DisplayColorMode::Hdr
        && (!capabilities->displayPathResolved
            || capabilities->advancedColorActive);
    const bafx::windows::CompositionOutputState& output =
        renderer.outputState();
    const bafx::windows::CompositionOutputPolicy resolvedPolicy =
        bafx::desktop::resolveDisplayOutputPolicy(
            displaySession.requestedOutputPreference(),
            capabilities);

    std::vector<bafx::windows::DisplaySessionRuntimeSummary>
        sessionSummaries;
    sessionSummaries.reserve(displaySessions.sessions().size());
    for (const auto& ownedSession : displaySessions.sessions())
    {
        const bafx::desktop::DisplayTarget& target =
            ownedSession->target();
        const auto& sessionCapabilities =
            ownedSession->colorCapabilities();
        const bafx::windows::CompositionOutputPolicy sessionPolicy =
            bafx::desktop::resolveDisplayOutputPolicy(
                ownedSession->requestedOutputPreference(),
                sessionCapabilities);
        const bafx::windows::CompositionOutputState& sessionOutput =
            ownedSession->renderer().outputState();
        const bafx::windows::BackgroundCadenceRefreshResult cadence =
            ownedSession->renderer().backgroundCaptureCadence();

        bafx::windows::DisplaySessionRuntimeSummary summary{};
        summary.monitor =
            bafx::desktop::formatDisplayTargetMonitor(target);
        summary.device = bafx::desktop::displayTargetDeviceUtf8(target);
        summary.displayKey =
            bafx::desktop::displayTargetPersistentKey(target);
        summary.bounds = target.bounds;
        summary.targetDpiX = target.dpiX;
        summary.targetDpiY = target.dpiY;
        summary.windowDpi = ownedSession->window().effectiveDpi();
        summary.displayRefreshRate = target.refreshRate;
        summary.captureRefreshRate = target.captureRefreshRate;
        summary.captureCadenceFallbackReason =
            target.captureCadenceFallbackReason;
        summary.captureCadenceStatus = cadence.status;
        summary.producerPolicyRefreshRate =
            cadence.producerPolicyRefreshRate;
        summary.freshnessPolicyRefreshRate =
            cadence.freshnessPolicyRefreshRate;
        summary.freshnessPolicyPeriod = cadence.appliedPeriod;
        summary.producerCadence = cadence.producerCadence;
        summary.physicalCadence.reserve(
            target.physicalTargetIdentities.size());
        for (const bafx::desktop::DisplayPhysicalTargetIdentity&
                physicalTarget : target.physicalTargetIdentities)
        {
            summary.physicalCadence.push_back(
                bafx::windows::DisplayPhysicalCadenceRuntimeSummary{
                    physicalTarget.virtualRefreshRate,
                    physicalTarget.physicalRefreshRate,
                    physicalTarget.captureRefreshRate,
                    physicalTarget.dynamicRefreshRateBoosted,
                    physicalTarget.available});
        }
        summary.sourceAdapterLuid = target.sourceAdapterLuid;
        summary.sourceId = target.sourceId;
        summary.physicalTargetCount = target.physicalTargetCount;
        summary.deviceInfo = ownedSession->renderer().deviceInfo();
        summary.requestedOutputPreference =
            ownedSession->requestedOutputPreference();
        summary.resolvedOutputPolicy = sessionPolicy;
        summary.colorCapabilities = sessionCapabilities;
        summary.colorObservation = ownedSession->colorObservation();
        summary.colorMonitorResult =
            ownedSession->colorMonitorResult();
        summary.colorSnapshotDisposition = std::string(
            bafx::desktop::displaySessionColorRefreshStatusName(
                ownedSession->colorSnapshotStatus()));
        summary.colorQueryGeneration =
            ownedSession->colorQueryGeneration();
        summary.backgroundCaptureFailure = std::string(
            ownedSession->renderer().backgroundCaptureFailure());
        summary.framePacing = std::string(
            bafx::config::toString(ownedSession->framePacing()));
        summary.effectsEnabled = ownedSession->effectsEnabled();
        summary.hdrEnabled = ownedSession->requestedOutputPreference()
            == bafx::windows::CompositionOutputPreference::PreferLinearScRgb;
        summary.coordinator = ownedSession.get() == &displaySession;
        summary.primary = target.primary;
        summary.sourceAdapterResolved = target.sourceAdapterResolved;
        summary.sourceIdentityResolved = target.sourceIdentityResolved;
        summary.sourceTopologyStatus = target.topologyStatus;
        summary.sourceTopologyError = target.topologyError;
        summary.colorRefreshRetriesRemaining =
            ownedSession->colorRefreshRetriesRemaining();
        summary.outputPolicySatisfied =
            ownedSession->renderer().outputPolicy() == sessionPolicy
            && bafx::windows::compositionOutputSatisfiesPolicy(
                sessionOutput,
                sessionPolicy);
        summary.backgroundCaptureActive =
            ownedSession->renderer().backgroundCaptureActive();
        summary.backgroundCaptureRestartAllowed =
            ownedSession->renderer().backgroundCaptureRestartAllowed();
        summary.renderFaulted = ownedSession->renderFaulted();
        summary.outputContractFaulted =
            ownedSession->outputContractFaulted();
        summary.activeFxRoi =
            ownedSession->activeFxRoiRuntimeSummary(runtimeObservedAt);
        sessionSummaries.push_back(std::move(summary));
    }
    std::sort(
        sessionSummaries.begin(),
        sessionSummaries.end(),
        [](const auto& left, const auto& right)
        {
            if (left.coordinator != right.coordinator)
            {
                return left.coordinator;
            }
            if (left.bounds.top != right.bounds.top)
            {
                return left.bounds.top < right.bounds.top;
            }
            if (left.bounds.left != right.bounds.left)
            {
                return left.bounds.left < right.bounds.left;
            }
            if (left.bounds.bottom != right.bounds.bottom)
            {
                return left.bounds.bottom < right.bounds.bottom;
            }
            if (left.bounds.right != right.bounds.right)
            {
                return left.bounds.right < right.bounds.right;
            }
            return left.device < right.device;
        });
    const std::size_t sessionCount = sessionSummaries.size();
    bafx::windows::DisplayRuntimeSummary runtimeSummary{};
    runtimeSummary.sessionCount = sessionCount;
    runtimeSummary.requestedOutputPreference =
        displaySession.requestedOutputPreference();
    runtimeSummary.resolvedOutputPreference = resolvedPolicy.preference;
    runtimeSummary.actualOutputPreference =
        bafx::windows::effectiveCompositionOutputPreference(output);
    runtimeSummary.outputPolicySatisfied =
        renderer.outputPolicy() == resolvedPolicy
        && bafx::windows::compositionOutputSatisfiesPolicy(
            output,
            resolvedPolicy);
    runtimeSummary.colorSnapshotComplete = colorSnapshotComplete;
    runtimeSummary.hdrCapabilityObserved = hdrCapabilityObserved;
    runtimeSummary.hdrActive = hdrActive;
    runtimeSummary.topologyStatus = latestDisplayTopologyStatus;
    runtimeSummary.topologyError = latestDisplayTopologyError;
    runtimeSummary.sessions = std::move(sessionSummaries);
    return runtimeSummary;
}

}
