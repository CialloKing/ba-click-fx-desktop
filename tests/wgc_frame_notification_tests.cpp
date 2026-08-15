#include "test_support.hpp"

#include "bafx/windows/detail/wgc_frame_notification.hpp"
#include "bafx/windows/wgc_background_sensor.hpp"

#include <windows.h>

#include <chrono>
#include <string>

using bafx::windows::detail::WgcFrameNotification;
using bafx::windows::detail::WgcBackgroundStopMailbox;
using bafx::windows::WgcBackgroundResourceLedger;
using bafx::windows::WgcBackgroundResourceLedgerSnapshot;

namespace
{

[[nodiscard]] DWORD signalState(const WgcFrameNotification& notification)
{
    return WaitForSingleObject(notification.eventObject(), 0U);
}

}

BAFX_TEST(wgc_notification_clears_after_the_observed_queue_is_empty)
{
    WgcFrameNotification notification;
    BAFX_CHECK(signalState(notification) == WAIT_TIMEOUT);

    notification.notifyFrame();
    const std::uint64_t observedGeneration = notification.generation();
    BAFX_CHECK(signalState(notification) == WAIT_OBJECT_0);

    notification.resetAfterDrain(observedGeneration, false);
    BAFX_CHECK(signalState(notification) == WAIT_TIMEOUT);
}

BAFX_TEST(wgc_notification_preserves_a_callback_that_precedes_reset)
{
    WgcFrameNotification notification;
    const std::uint64_t observedGeneration = notification.generation();

    notification.notifyFrame();
    notification.resetAfterDrain(observedGeneration, false);

    BAFX_CHECK(signalState(notification) == WAIT_OBJECT_0);
}

BAFX_TEST(wgc_notification_signals_for_a_callback_after_reset)
{
    WgcFrameNotification notification;
    const std::uint64_t observedGeneration = notification.generation();

    notification.resetAfterDrain(observedGeneration, false);
    notification.notifyFrame();

    BAFX_CHECK(signalState(notification) == WAIT_OBJECT_0);
}

BAFX_TEST(wgc_notification_keeps_bounded_backlog_runnable)
{
    WgcFrameNotification notification;
    const std::uint64_t observedGeneration = notification.generation();

    notification.resetAfterDrain(observedGeneration, true);
    BAFX_CHECK(signalState(notification) == WAIT_OBJECT_0);

    // The next empty drain removes the conservative extra wake.
    notification.resetAfterDrain(observedGeneration, false);
    BAFX_CHECK(signalState(notification) == WAIT_TIMEOUT);
}

BAFX_TEST(wgc_notification_keeps_item_close_visible_to_the_owner)
{
    WgcFrameNotification notification;
    const std::uint64_t observedGeneration = notification.generation();

    notification.notifyFrame();
    BAFX_CHECK(!notification.itemClosed());

    notification.notifyItemClosed();
    // The Host can observe close from its control poll without first draining
    // a frame, which is required while paused with no drawable FX content.
    BAFX_CHECK(notification.itemClosed());
    notification.resetAfterDrain(observedGeneration, false);

    BAFX_CHECK(notification.itemClosed());
    BAFX_CHECK(signalState(notification) == WAIT_OBJECT_0);
}

BAFX_TEST(wgc_notification_stop_wakes_the_owner_and_ignores_late_frames)
{
    WgcFrameNotification notification;
    const std::uint64_t generationBeforeStop = notification.generation();

    notification.beginStop();
    notification.notifyFrame();

    BAFX_CHECK(notification.generation() == generationBeforeStop);
    BAFX_CHECK(signalState(notification) == WAIT_OBJECT_0);
}

BAFX_TEST(wgc_notification_reset_cannot_clear_a_stop_wakeup)
{
    WgcFrameNotification notification;
    const std::uint64_t observedGeneration = notification.generation();

    notification.beginStop();
    notification.resetAfterDrain(observedGeneration, false);

    // A stop request is a terminal owner wake even when no frame callback was
    // queued.  Losing it would leave the lifecycle transaction asleep.
    BAFX_CHECK(signalState(notification) == WAIT_OBJECT_0);
}

BAFX_TEST(wgc_resource_ledger_starts_empty_and_released)
{
    const WgcBackgroundResourceLedger ledger;
    const WgcBackgroundResourceLedgerSnapshot snapshot = ledger.snapshot();

    BAFX_CHECK(snapshot.allReleased());
    BAFX_CHECK(snapshot.framesAcquired == 0U);
    BAFX_CHECK(snapshot.framePoolsCreated == 0U);
    BAFX_CHECK(snapshot.sessionsCreated == 0U);
    BAFX_CHECK(snapshot.failures == 0U);
}

BAFX_TEST(wgc_resource_ledger_requires_every_live_resource_to_be_released)
{
    WgcBackgroundResourceLedgerSnapshot snapshot{};
    snapshot.liveFrames = 1U;
    BAFX_CHECK(!snapshot.allReleased());

    snapshot.liveFrames = 0U;
    snapshot.liveFramePools = 1U;
    BAFX_CHECK(!snapshot.allReleased());

    snapshot.liveFramePools = 0U;
    snapshot.liveSessions = 1U;
    BAFX_CHECK(!snapshot.allReleased());

    snapshot.liveSessions = 0U;
    snapshot.liveFrameArrivedRegistrations = 1U;
    BAFX_CHECK(!snapshot.allReleased());

    snapshot.liveFrameArrivedRegistrations = 0U;
    snapshot.liveItemClosedRegistrations = 1U;
    BAFX_CHECK(!snapshot.allReleased());
}

BAFX_TEST(wgc_resource_ledger_diagnostic_includes_release_and_failure_evidence)
{
    WgcBackgroundResourceLedgerSnapshot snapshot{};
    snapshot.framesAcquired = 4U;
    snapshot.framesClosed = 3U;
    snapshot.framePoolsCreated = 2U;
    snapshot.framePoolsClosed = 1U;
    snapshot.framePoolsRecreated = 1U;
    snapshot.sessionsCreated = 2U;
    snapshot.sessionsClosed = 1U;
    snapshot.liveFrames = 1U;
    snapshot.liveFramePools = 1U;
    snapshot.liveSessions = 1U;
    snapshot.failures = 2U;

    const std::string diagnostic =
        bafx::windows::wgcBackgroundResourceLedgerDiagnostic(snapshot);
    BAFX_CHECK(
        diagnostic.find("WGC.ResourceLedger.FramesAcquired=4")
        != std::string::npos);
    BAFX_CHECK(diagnostic.find("FramesClosed=3") != std::string::npos);
    BAFX_CHECK(diagnostic.find("FramePoolsRecreated=1") != std::string::npos);
    BAFX_CHECK(diagnostic.find("SessionsClosed=1") != std::string::npos);
    BAFX_CHECK(diagnostic.find("LiveFramePools=1") != std::string::npos);
    BAFX_CHECK(diagnostic.find("Failures=2") != std::string::npos);
    BAFX_CHECK(diagnostic.find("AllReleased=false") != std::string::npos);
}

BAFX_TEST(wgc_stop_diagnostic_reports_each_uncancellable_phase)
{
    bafx::windows::WgcBackgroundStopDiagnostics diagnostics{};
    diagnostics.frameArrivedUnregister = std::chrono::microseconds(11);
    diagnostics.itemClosedUnregister = std::chrono::microseconds(22);
    diagnostics.sessionClose = std::chrono::microseconds(33);
    diagnostics.framePoolClose = std::chrono::microseconds(44);
    diagnostics.total = std::chrono::microseconds(123);
    diagnostics.sensorPresent = true;
    diagnostics.itemClosedUnregisterFailed = true;
    diagnostics.sessionCloseFailed = true;
    diagnostics.completed = true;
    diagnostics.overallSucceeded = false;

    const std::string text =
        bafx::windows::wgcBackgroundStopDiagnostic(diagnostics);
    BAFX_CHECK(text.find("WGC.Stop.SensorPresent=true") != std::string::npos);
    BAFX_CHECK(text.find("FrameArrivedUnregisterFailed=false")
        != std::string::npos);
    BAFX_CHECK(text.find("ItemClosedUnregisterFailed=true")
        != std::string::npos);
    BAFX_CHECK(text.find("SessionCloseFailed=true") != std::string::npos);
    BAFX_CHECK(text.find("FramePoolCloseFailed=false") != std::string::npos);
    BAFX_CHECK(text.find("WGC.Stop.Completed=true") != std::string::npos);
    BAFX_CHECK(text.find("WGC.Stop.OverallSucceeded=false")
        != std::string::npos);
    BAFX_CHECK(text.find("WGC.Stop.DeferredReport=false") != std::string::npos);
    BAFX_CHECK(text.find("FrameArrivedUnregisterUs=11") != std::string::npos);
    BAFX_CHECK(text.find("ItemClosedUnregisterUs=22") != std::string::npos);
    BAFX_CHECK(text.find("SessionCloseUs=33") != std::string::npos);
    BAFX_CHECK(text.find("FramePoolCloseUs=44") != std::string::npos);
    BAFX_CHECK(text.find("WGC.Stop.TotalUs=123") != std::string::npos);
}

BAFX_TEST(wgc_stop_mailbox_preserves_a_sensor_stop_across_cleanup_noop)
{
    WgcBackgroundStopMailbox mailbox;
    bafx::windows::WgcBackgroundStopDiagnostics diagnostics{};
    diagnostics.sessionClose = std::chrono::microseconds(37);
    diagnostics.total = std::chrono::microseconds(51);
    diagnostics.sensorPresent = true;
    diagnostics.sessionCloseFailed = true;
    diagnostics.completed = true;
    diagnostics.overallSucceeded = false;

    mailbox.record(diagnostics);
    mailbox.recordNoSensor();
    const bafx::windows::WgcBackgroundStopDiagnostics preserved =
        mailbox.take();

    BAFX_CHECK(preserved.sensorPresent);
    BAFX_CHECK(preserved.completed);
    BAFX_CHECK(preserved.sessionCloseFailed);
    BAFX_CHECK(!preserved.overallSucceeded);
    BAFX_CHECK(preserved.deferredReport);
    BAFX_CHECK(preserved.sessionClose == std::chrono::microseconds(37));
    BAFX_CHECK(preserved.total == std::chrono::microseconds(51));
}

BAFX_TEST(wgc_stop_mailbox_does_not_reuse_consumed_sensor_evidence)
{
    WgcBackgroundStopMailbox mailbox;
    bafx::windows::WgcBackgroundStopDiagnostics diagnostics{};
    diagnostics.total = std::chrono::microseconds(51);
    diagnostics.sensorPresent = true;
    diagnostics.completed = true;
    diagnostics.overallSucceeded = true;

    mailbox.record(diagnostics);
    static_cast<void>(mailbox.take());
    mailbox.recordNoSensor();
    const bafx::windows::WgcBackgroundStopDiagnostics next = mailbox.take();

    BAFX_CHECK(!next.sensorPresent);
    BAFX_CHECK(next.completed);
    BAFX_CHECK(next.overallSucceeded);
    BAFX_CHECK(!next.deferredReport);
    BAFX_CHECK(next.total == std::chrono::nanoseconds::zero());
}
