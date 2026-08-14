#include "spike_runtime.hpp"

#include "bafx/windows/error.hpp"

#include <objbase.h>

#include <algorithm>

namespace bafx::capture
{

ComApartment::ComApartment()
{
    const HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (result == RPC_E_CHANGED_MODE)
    {
        throw bafx::windows::HResultError(result, "CoInitializeEx");
    }
    bafx::windows::throwIfFailed(result, "CoInitializeEx");
    initialized_ = true;
}

ComApartment::~ComApartment()
{
    if (initialized_)
    {
        CoUninitialize();
    }
}

ProcessWatchdog::ProcessWatchdog(const std::uint32_t timeoutMilliseconds)
    : stopEvent_(CreateEventW(nullptr, TRUE, FALSE, nullptr))
{
    if (stopEvent_.get() == nullptr)
    {
        bafx::windows::throwLastError("CreateEventW(spike watchdog)");
    }

    const HANDLE stopEvent = stopEvent_.get();
    worker_ = std::thread(
        [stopEvent, timeoutMilliseconds]() noexcept
        {
            const DWORD result = WaitForSingleObject(
                stopEvent,
                timeoutMilliseconds);
            if (result == WAIT_TIMEOUT)
            {
                // Driver and WinRT calls can block below the cooperative
                // deadline, so every disposable hardware probe needs a hard stop.
                if (!TerminateProcess(GetCurrentProcess(), 124U))
                {
                    ExitProcess(124U);
                }
            }
        });
}

ProcessWatchdog::~ProcessWatchdog()
{
    if (stopEvent_.get() != nullptr)
    {
        SetEvent(stopEvent_.get());
    }
    if (worker_.joinable())
    {
        worker_.join();
    }
}

Deadline::Deadline(const std::chrono::milliseconds duration)
    : expiresAt_(std::chrono::steady_clock::now() + duration)
{
}

bool Deadline::expired() const noexcept
{
    return std::chrono::steady_clock::now() >= expiresAt_;
}

DWORD Deadline::nextWaitMilliseconds() const noexcept
{
    const auto now = std::chrono::steady_clock::now();
    if (now >= expiresAt_)
    {
        return 0U;
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        expiresAt_ - now);
    const auto bounded = std::clamp<std::int64_t>(
        remaining.count(),
        1,
        50);
    return static_cast<DWORD>(bounded);
}

}
