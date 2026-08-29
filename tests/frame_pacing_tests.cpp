#include "test_support.hpp"

#include "frame_pacing.hpp"

#include <windows.h>

#include <stdexcept>

namespace
{

using bafx::desktop::FramePacingWake;
using bafx::desktop::PausedWaitWake;
using bafx::desktop::nextFrameStartDeadlineAfterPresent;
using bafx::desktop::waitForFrameOpportunity;
using bafx::desktop::waitForPausedInvalidation;

class EventHandle final
{
public:
    explicit EventHandle(const bool signaled)
        : handle_(CreateEventW(nullptr, FALSE, signaled ? TRUE : FALSE, nullptr))
    {
        if (handle_ == nullptr)
        {
            throw std::runtime_error("CreateEventW failed");
        }
    }

    ~EventHandle()
    {
        if (handle_ != nullptr)
        {
            CloseHandle(handle_);
        }
    }

    EventHandle(const EventHandle&) = delete;
    EventHandle& operator=(const EventHandle&) = delete;

    [[nodiscard]] HANDLE get() const noexcept
    {
        return handle_;
    }

    void close() noexcept
    {
        CloseHandle(handle_);
        handle_ = nullptr;
    }

private:
    HANDLE handle_{nullptr};
};

constexpr UINT pacingTestMessage = WM_APP + 71U;

}

BAFX_TEST(frame_pacing_anchors_the_next_start_to_present_completion)
{
    const auto deadline = nextFrameStartDeadlineAfterPresent(
        std::chrono::milliseconds(10),
        std::chrono::nanoseconds(5'882'353),
        std::chrono::microseconds(50));
    BAFX_CHECK(deadline == std::chrono::nanoseconds(15'582'353));
}

BAFX_TEST(frame_pacing_never_delays_a_frame_whose_preparation_fills_the_period)
{
    const auto presentedAt = std::chrono::milliseconds(10);
    BAFX_CHECK(
        nextFrameStartDeadlineAfterPresent(
            presentedAt,
            std::chrono::milliseconds(5),
            std::chrono::milliseconds(5))
        == presentedAt);
    BAFX_CHECK(
        nextFrameStartDeadlineAfterPresent(
            presentedAt,
            std::chrono::milliseconds(5),
            std::chrono::milliseconds(7))
        == presentedAt);
}

BAFX_TEST(frame_pacing_clamps_invalid_durations_and_deadline_overflow)
{
    BAFX_CHECK(
        nextFrameStartDeadlineAfterPresent(
            std::chrono::nanoseconds(1),
            std::chrono::milliseconds(1),
            std::chrono::microseconds(-1))
        == std::chrono::microseconds(750) + std::chrono::nanoseconds(1));
    BAFX_CHECK(
        nextFrameStartDeadlineAfterPresent(
            (std::chrono::nanoseconds::max)() - std::chrono::nanoseconds(1),
            std::chrono::milliseconds(1),
            std::chrono::nanoseconds::zero())
        == (std::chrono::nanoseconds::max)());
}

BAFX_TEST(frame_pacing_grants_render_only_for_the_latency_object)
{
    EventHandle frameReady(true);
    const auto result = waitForFrameOpportunity(frameReady.get(), nullptr, 0U);
    BAFX_CHECK(result.wake == FramePacingWake::FrameReady);
    BAFX_CHECK(result.error == ERROR_SUCCESS);
}

BAFX_TEST(frame_pacing_keeps_message_wakes_separate_from_render_slots)
{
    MSG message{};
    static_cast<void>(PeekMessageW(
        &message,
        nullptr,
        pacingTestMessage,
        pacingTestMessage,
        PM_NOREMOVE));
    BAFX_CHECK(PostThreadMessageW(
        GetCurrentThreadId(),
        pacingTestMessage,
        0U,
        0) != FALSE);

    EventHandle frameReady(false);
    EventHandle deviceRemoved(false);
    const auto result = waitForFrameOpportunity(
        frameReady.get(),
        deviceRemoved.get(),
        0U);
    BAFX_CHECK(result.wake == FramePacingWake::MessagesPending);
    BAFX_CHECK(result.error == ERROR_SUCCESS);
    BAFX_CHECK(PeekMessageW(
        &message,
        nullptr,
        pacingTestMessage,
        pacingTestMessage,
        PM_REMOVE) != FALSE);
}

BAFX_TEST(frame_pacing_reports_timeout_and_invalid_handle)
{
    EventHandle frameReady(false);
    const auto timeout = waitForFrameOpportunity(frameReady.get(), nullptr, 0U);
    BAFX_CHECK(timeout.wake == FramePacingWake::TimedOut);
    BAFX_CHECK(timeout.error == ERROR_SUCCESS);

    const auto invalid = waitForFrameOpportunity(nullptr, nullptr, 0U);
    BAFX_CHECK(invalid.wake == FramePacingWake::Failed);
    BAFX_CHECK(invalid.error == ERROR_INVALID_HANDLE);
}

BAFX_TEST(frame_pacing_preserves_the_error_from_a_closed_handle)
{
    EventHandle frameReady(false);
    const HANDLE closed = frameReady.get();
    frameReady.close();

    const auto result = waitForFrameOpportunity(closed, nullptr, 0U);
    BAFX_CHECK(result.wake == FramePacingWake::Failed);
    BAFX_CHECK(result.error == ERROR_INVALID_HANDLE);
}

BAFX_TEST(frame_pacing_prioritizes_device_removal_over_a_ready_frame)
{
    EventHandle frameReady(true);
    EventHandle deviceRemoved(true);

    const auto result = waitForFrameOpportunity(
        frameReady.get(),
        deviceRemoved.get(),
        0U);
    BAFX_CHECK(result.wake == FramePacingWake::DeviceRemoved);
    BAFX_CHECK(result.error == ERROR_SUCCESS);
}

BAFX_TEST(frame_pacing_rejects_an_invalid_optional_device_handle)
{
    EventHandle frameReady(true);
    const auto result = waitForFrameOpportunity(
        frameReady.get(),
        INVALID_HANDLE_VALUE,
        0U);
    BAFX_CHECK(result.wake == FramePacingWake::Failed);
    BAFX_CHECK(result.error == ERROR_INVALID_HANDLE);
}

BAFX_TEST(paused_wait_prioritizes_device_removal_over_a_background_frame)
{
    EventHandle deviceRemoved(true);
    EventHandle backgroundFrame(true);

    const auto result = waitForPausedInvalidation(
        deviceRemoved.get(),
        backgroundFrame.get(),
        0U);
    BAFX_CHECK(result.wake == PausedWaitWake::DeviceRemoved);
    BAFX_CHECK(result.error == ERROR_SUCCESS);
}

BAFX_TEST(paused_wait_reports_a_background_frame_without_device_notification)
{
    EventHandle backgroundFrame(true);

    const auto result = waitForPausedInvalidation(
        nullptr,
        backgroundFrame.get(),
        0U);
    BAFX_CHECK(result.wake == PausedWaitWake::BackgroundFrameReady);
    BAFX_CHECK(result.error == ERROR_SUCCESS);
}

BAFX_TEST(paused_wait_allows_no_optional_handles)
{
    const auto result = waitForPausedInvalidation(nullptr, nullptr, 0U);
    BAFX_CHECK(result.wake == PausedWaitWake::TimedOut);
    BAFX_CHECK(result.error == ERROR_SUCCESS);
}

BAFX_TEST(paused_wait_reports_messages_without_optional_handles)
{
    MSG message{};
    static_cast<void>(PeekMessageW(
        &message,
        nullptr,
        pacingTestMessage,
        pacingTestMessage,
        PM_NOREMOVE));
    BAFX_CHECK(PostThreadMessageW(
        GetCurrentThreadId(),
        pacingTestMessage,
        0U,
        0) != FALSE);

    const auto result = waitForPausedInvalidation(nullptr, nullptr, 0U);
    BAFX_CHECK(result.wake == PausedWaitWake::MessagesPending);
    BAFX_CHECK(result.error == ERROR_SUCCESS);
    BAFX_CHECK(PeekMessageW(
        &message,
        nullptr,
        pacingTestMessage,
        pacingTestMessage,
        PM_REMOVE) != FALSE);
}

BAFX_TEST(paused_wait_rejects_invalid_optional_handles)
{
    EventHandle backgroundFrame(false);
    const auto invalidDevice = waitForPausedInvalidation(
        INVALID_HANDLE_VALUE,
        backgroundFrame.get(),
        0U);
    BAFX_CHECK(invalidDevice.wake == PausedWaitWake::Failed);
    BAFX_CHECK(invalidDevice.error == ERROR_INVALID_HANDLE);

    EventHandle deviceRemoved(false);
    const auto invalidBackground = waitForPausedInvalidation(
        deviceRemoved.get(),
        INVALID_HANDLE_VALUE,
        0U);
    BAFX_CHECK(invalidBackground.wake == PausedWaitWake::Failed);
    BAFX_CHECK(invalidBackground.error == ERROR_INVALID_HANDLE);
}
