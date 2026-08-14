#include "test_support.hpp"

#include "bafx/windows/detail/wgc_frame_notification.hpp"

#include <windows.h>

using bafx::windows::detail::WgcFrameNotification;

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
