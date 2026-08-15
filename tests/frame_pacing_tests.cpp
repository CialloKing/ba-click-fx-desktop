#include "test_support.hpp"

#include "frame_pacing.hpp"

#include <windows.h>

#include <stdexcept>

namespace
{

using bafx::desktop::FramePacingWake;
using bafx::desktop::waitForFrameOpportunity;

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

BAFX_TEST(frame_pacing_grants_render_only_for_the_latency_object)
{
    EventHandle frameReady(true);
    const auto result = waitForFrameOpportunity(frameReady.get(), 0U);
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
    const auto result = waitForFrameOpportunity(frameReady.get(), 0U);
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
    const auto timeout = waitForFrameOpportunity(frameReady.get(), 0U);
    BAFX_CHECK(timeout.wake == FramePacingWake::TimedOut);
    BAFX_CHECK(timeout.error == ERROR_SUCCESS);

    const auto invalid = waitForFrameOpportunity(nullptr, 0U);
    BAFX_CHECK(invalid.wake == FramePacingWake::Failed);
    BAFX_CHECK(invalid.error == ERROR_INVALID_HANDLE);
}

BAFX_TEST(frame_pacing_preserves_the_error_from_a_closed_handle)
{
    EventHandle frameReady(false);
    const HANDLE closed = frameReady.get();
    frameReady.close();

    const auto result = waitForFrameOpportunity(closed, 0U);
    BAFX_CHECK(result.wake == FramePacingWake::Failed);
    BAFX_CHECK(result.error == ERROR_INVALID_HANDLE);
}
