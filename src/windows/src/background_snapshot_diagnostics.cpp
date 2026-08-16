#include "bafx/windows/background_snapshot_diagnostics.hpp"

namespace bafx::windows
{

std::string_view backgroundSnapshotInvalidationReasonName(
    const BackgroundSnapshotInvalidationReason reason) noexcept
{
    switch (reason)
    {
    case BackgroundSnapshotInvalidationReason::VisibleBatchEnded:
        return "visible-batch-ended";
    case BackgroundSnapshotInvalidationReason::FxOnlyPathSelected:
        return "fx-only-path-selected";
    case BackgroundSnapshotInvalidationReason::WgcDrainFailed:
        return "wgc-drain-failed";
    case BackgroundSnapshotInvalidationReason::WgcSessionStopped:
        return "wgc-session-stopped";
    case BackgroundSnapshotInvalidationReason::FramePoolReconfigureRequired:
        return "frame-pool-reconfigure-required";
    case BackgroundSnapshotInvalidationReason::OutputResize:
        return "output-resize";
    case BackgroundSnapshotInvalidationReason::DeviceResourcesReleased:
        return "device-resources-released";
    case BackgroundSnapshotInvalidationReason::CaptureSessionReplaced:
        return "capture-session-replaced";
    case BackgroundSnapshotInvalidationReason::CaptureDisabled:
        return "capture-disabled";
    case BackgroundSnapshotInvalidationReason::SensorStartFailed:
        return "sensor-start-failed";
    case BackgroundSnapshotInvalidationReason::ReferenceWhiteUnavailable:
        return "reference-white-unavailable";
    case BackgroundSnapshotInvalidationReason::SnapshotResourcesRecreated:
        return "snapshot-resources-recreated";
    }
    return "unknown";
}

void detail::BackgroundSnapshotInvalidationMailbox::record(
    const BackgroundSnapshotInvalidation invalidation) noexcept
{
    if (pending_.has_value())
    {
        // Cleanup can call reset and release back-to-back. Keep the first
        // state edge because it carries the causal reason and old identity.
        return;
    }
    pending_ = invalidation;
}

std::optional<BackgroundSnapshotInvalidation>
detail::BackgroundSnapshotInvalidationMailbox::take() noexcept
{
    const std::optional<BackgroundSnapshotInvalidation> result = pending_;
    pending_.reset();
    return result;
}

}
