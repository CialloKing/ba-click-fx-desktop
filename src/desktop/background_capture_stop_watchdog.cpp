#include "background_capture_stop_watchdog.hpp"

#include <windows.h>

#include <stdexcept>

namespace bafx::desktop
{
namespace
{

[[nodiscard]] std::chrono::milliseconds checkedTimeout(
    const std::chrono::milliseconds timeout)
{
    if (timeout <= std::chrono::milliseconds::zero())
    {
        throw std::invalid_argument(
            "Background capture stop watchdog requires a positive timeout");
    }
    return timeout;
}

}

BackgroundCaptureStopWatchdog::BackgroundCaptureStopWatchdog(
    const std::chrono::milliseconds timeout,
    const BackgroundCaptureStopTimeoutHandler timeoutHandler,
    const void* const timeoutContext)
    : timeout_(checkedTimeout(timeout)),
      timeoutHandler_(timeoutHandler != nullptr
              ? timeoutHandler
              : &BackgroundCaptureStopWatchdog::terminateCurrentProcess),
      timeoutContext_(timeoutContext),
      worker_([this]() noexcept
      {
          run();
      })
{
}

BackgroundCaptureStopWatchdog::~BackgroundCaptureStopWatchdog()
{
    {
        const std::lock_guard lock(mutex_);
        stopping_ = true;
        armed_ = false;
        ++generation_;
    }
    condition_.notify_all();
    if (worker_.joinable())
    {
        worker_.join();
    }
}

bool BackgroundCaptureStopWatchdog::arm() noexcept
{
    {
        const std::lock_guard lock(mutex_);
        if (stopping_ || armed_)
        {
            return false;
        }
        armed_ = true;
        ++generation_;
    }
    condition_.notify_all();
    return true;
}

void BackgroundCaptureStopWatchdog::disarm() noexcept
{
    {
        const std::lock_guard lock(mutex_);
        if (!armed_)
        {
            return;
        }
        armed_ = false;
        ++generation_;
    }
    condition_.notify_all();
}

bool BackgroundCaptureStopWatchdog::armed() const noexcept
{
    const std::lock_guard lock(mutex_);
    return armed_;
}

std::chrono::milliseconds BackgroundCaptureStopWatchdog::timeout() const noexcept
{
    return timeout_;
}

void BackgroundCaptureStopWatchdog::terminateCurrentProcess(
    const void*) noexcept
{
    if (!TerminateProcess(
            GetCurrentProcess(),
            backgroundCaptureStopTimeoutExitCode))
    {
        ExitProcess(backgroundCaptureStopTimeoutExitCode);
    }
}

void BackgroundCaptureStopWatchdog::run() noexcept
{
    std::unique_lock lock(mutex_);
    for (;;)
    {
        condition_.wait(lock, [this]() noexcept
        {
            return stopping_ || armed_;
        });
        if (stopping_)
        {
            return;
        }

        const std::uint64_t generation = generation_;
        const bool stateChanged = condition_.wait_for(
            lock,
            timeout_,
            [this, generation]() noexcept
            {
                return stopping_ || generation_ != generation;
            });
        if (stopping_)
        {
            return;
        }
        if (stateChanged)
        {
            continue;
        }

        // Clear the arm before invoking an injectable test handler. The
        // production handler terminates the process and does not return.
        armed_ = false;
        ++generation_;
        const BackgroundCaptureStopTimeoutHandler handler = timeoutHandler_;
        const void* const context = timeoutContext_;
        lock.unlock();
        handler(context);
        lock.lock();
    }
}

}
