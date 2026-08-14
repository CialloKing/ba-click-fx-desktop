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
        CloseHandle(handle_);
    }

    EventHandle(const EventHandle&) = delete;
    EventHandle& operator=(const EventHandle&) = delete;

    [[nodiscard]] HANDLE get() const noexcept
    {
        return handle_;
    }

private:
    HANDLE handle_{nullptr};
};

constexpr UINT pacingTestMessage = WM_APP + 71U;

}

BAFX_TEST(frame_pacing_grants_render_only_for_the_latency_object)
{
    EventHandle frameReady(true);
    BAFX_CHECK(waitForFrameOpportunity(frameReady.get(), 0U)
        == FramePacingWake::FrameReady);
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
    BAFX_CHECK(waitForFrameOpportunity(frameReady.get(), 0U)
        == FramePacingWake::MessagesPending);
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
    BAFX_CHECK(waitForFrameOpportunity(frameReady.get(), 0U)
        == FramePacingWake::TimedOut);
    BAFX_CHECK(waitForFrameOpportunity(nullptr, 0U)
        == FramePacingWake::Failed);
    BAFX_CHECK(GetLastError() == ERROR_INVALID_HANDLE);
}
