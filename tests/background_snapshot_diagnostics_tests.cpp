#include "test_support.hpp"

#include "bafx/windows/background_snapshot_diagnostics.hpp"

#include <array>
#include <string_view>
#include <utility>

using bafx::windows::BackgroundSnapshotInvalidation;
using bafx::windows::BackgroundSnapshotInvalidationReason;
using bafx::windows::detail::BackgroundSnapshotInvalidationMailbox;

BAFX_TEST(background_snapshot_invalidation_reason_names_are_stable)
{
    const std::array cases{
        std::pair{BackgroundSnapshotInvalidationReason::VisibleBatchEnded,
                  std::string_view("visible-batch-ended")},
        std::pair{BackgroundSnapshotInvalidationReason::FxOnlyPathSelected,
                  std::string_view("fx-only-path-selected")},
        std::pair{BackgroundSnapshotInvalidationReason::WgcDrainFailed,
                  std::string_view("wgc-drain-failed")},
        std::pair{BackgroundSnapshotInvalidationReason::WgcSessionStopped,
                  std::string_view("wgc-session-stopped")},
        std::pair{
            BackgroundSnapshotInvalidationReason::FramePoolReconfigureRequired,
            std::string_view("frame-pool-reconfigure-required")},
        std::pair{BackgroundSnapshotInvalidationReason::OutputResize,
                  std::string_view("output-resize")},
        std::pair{BackgroundSnapshotInvalidationReason::DeviceResourcesReleased,
                  std::string_view("device-resources-released")},
        std::pair{BackgroundSnapshotInvalidationReason::CaptureSessionReplaced,
                  std::string_view("capture-session-replaced")},
        std::pair{BackgroundSnapshotInvalidationReason::CaptureDisabled,
                  std::string_view("capture-disabled")},
        std::pair{BackgroundSnapshotInvalidationReason::SensorStartFailed,
                  std::string_view("sensor-start-failed")},
        std::pair{
            BackgroundSnapshotInvalidationReason::ReferenceWhiteUnavailable,
            std::string_view("reference-white-unavailable")},
        std::pair{
            BackgroundSnapshotInvalidationReason::SnapshotResourcesRecreated,
            std::string_view("snapshot-resources-recreated")}};

    for (const auto& [reason, expected] : cases)
    {
        BAFX_CHECK(
            bafx::windows::backgroundSnapshotInvalidationReasonName(reason)
            == expected);
    }
}

BAFX_TEST(background_snapshot_invalidation_mailbox_is_single_consumer)
{
    BackgroundSnapshotInvalidationMailbox mailbox;
    BAFX_CHECK(!mailbox.take().has_value());

    const BackgroundSnapshotInvalidation expected{
        BackgroundSnapshotInvalidationReason::CaptureDisabled,
        17U,
        23U,
        29U,
        31U,
        37U};
    mailbox.record(expected);

    const auto actual = mailbox.take();
    BAFX_CHECK(actual.has_value());
    BAFX_CHECK(actual->reason == expected.reason);
    BAFX_CHECK(actual->frameId == expected.frameId);
    BAFX_CHECK(actual->wgcEpoch == expected.wgcEpoch);
    BAFX_CHECK(actual->wgcGeneration == expected.wgcGeneration);
    BAFX_CHECK(actual->snapshotEpoch == expected.snapshotEpoch);
    BAFX_CHECK(actual->snapshotGeneration == expected.snapshotGeneration);
    BAFX_CHECK(!mailbox.take().has_value());
}

BAFX_TEST(background_snapshot_invalidation_mailbox_preserves_first_edge)
{
    BackgroundSnapshotInvalidationMailbox mailbox;
    mailbox.record(BackgroundSnapshotInvalidation{
        BackgroundSnapshotInvalidationReason::WgcDrainFailed,
        41U,
        43U,
        47U,
        53U,
        59U});
    mailbox.record(BackgroundSnapshotInvalidation{
        BackgroundSnapshotInvalidationReason::CaptureDisabled,
        61U,
        67U,
        71U,
        73U,
        79U});

    const auto first = mailbox.take();
    BAFX_CHECK(first.has_value());
    BAFX_CHECK(
        first->reason
        == BackgroundSnapshotInvalidationReason::WgcDrainFailed);
    BAFX_CHECK(first->frameId == 41U);

    mailbox.record(BackgroundSnapshotInvalidation{
        BackgroundSnapshotInvalidationReason::CaptureDisabled,
        61U,
        67U,
        71U,
        73U,
        79U});
    const auto second = mailbox.take();
    BAFX_CHECK(second.has_value());
    BAFX_CHECK(second->frameId == 61U);
}
