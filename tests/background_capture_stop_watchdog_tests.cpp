#include "test_support.hpp"

#include "background_capture_stop_watchdog.hpp"

#include <windows.h>

#include <atomic>
#include <chrono>

namespace
{

class TimeoutProbe final
{
public:
    TimeoutProbe()
        : event_(CreateEventW(nullptr, TRUE, FALSE, nullptr))
    {
    }

    ~TimeoutProbe()
    {
        if (event_ != nullptr)
        {
            CloseHandle(event_);
        }
    }

    TimeoutProbe(const TimeoutProbe&) = delete;
    TimeoutProbe& operator=(const TimeoutProbe&) = delete;

    static void trigger(const void* const context) noexcept
    {
        auto& probe = *static_cast<TimeoutProbe*>(const_cast<void*>(context));
        probe.count_.fetch_add(1U, std::memory_order_relaxed);
        SetEvent(probe.event_);
    }

    [[nodiscard]] HANDLE event() const noexcept
    {
        return event_;
    }

    [[nodiscard]] std::uint32_t count() const noexcept
    {
        return count_.load(std::memory_order_relaxed);
    }

private:
    HANDLE event_{nullptr};
    std::atomic<std::uint32_t> count_{0U};
};

}

BAFX_TEST(background_capture_stop_watchdog_disarm_cancels_the_deadline)
{
    TimeoutProbe probe;
    {
        bafx::desktop::BackgroundCaptureStopWatchdog watchdog(
            std::chrono::milliseconds(100),
            &TimeoutProbe::trigger,
            &probe);
        BAFX_CHECK(watchdog.arm());
        BAFX_CHECK(watchdog.armed());
        BAFX_CHECK(!watchdog.arm());
        watchdog.disarm();
        BAFX_CHECK(!watchdog.armed());
    }

    BAFX_CHECK(probe.count() == 0U);
    BAFX_CHECK(WaitForSingleObject(probe.event(), 0U) == WAIT_TIMEOUT);
}

BAFX_TEST(background_capture_stop_watchdog_fires_once_and_can_rearm)
{
    TimeoutProbe probe;
    bafx::desktop::BackgroundCaptureStopWatchdog watchdog(
        std::chrono::milliseconds(20),
        &TimeoutProbe::trigger,
        &probe);

    BAFX_CHECK(watchdog.arm());
    BAFX_CHECK(WaitForSingleObject(probe.event(), 1'000U) == WAIT_OBJECT_0);
    BAFX_CHECK(probe.count() == 1U);
    BAFX_CHECK(!watchdog.armed());

    ResetEvent(probe.event());
    BAFX_CHECK(watchdog.arm());
    BAFX_CHECK(WaitForSingleObject(probe.event(), 1'000U) == WAIT_OBJECT_0);
    BAFX_CHECK(probe.count() == 2U);
    BAFX_CHECK(!watchdog.armed());
}
