#include "frame_pacing.hpp"

#include <algorithm>
#include <array>
#include <limits>

namespace bafx::desktop
{
namespace
{

[[nodiscard]] FramePacingWake framePacingWake(
    const FramePacingWaitableKind kind) noexcept
{
    switch (kind)
    {
    case FramePacingWaitableKind::FrameReady:
        return FramePacingWake::FrameReady;
    case FramePacingWaitableKind::DeviceRemoved:
        return FramePacingWake::DeviceRemoved;
    case FramePacingWaitableKind::ControlChanged:
        return FramePacingWake::ControlChanged;
    case FramePacingWaitableKind::CadenceReady:
        return FramePacingWake::CadenceReady;
    }
    return FramePacingWake::Failed;
}

[[nodiscard]] PausedWaitWake pausedWaitWake(
    const PausedWaitableKind kind) noexcept
{
    switch (kind)
    {
    case PausedWaitableKind::DeviceRemoved:
        return PausedWaitWake::DeviceRemoved;
    case PausedWaitableKind::BackgroundFrameReady:
        return PausedWaitWake::BackgroundFrameReady;
    case PausedWaitableKind::ControlChanged:
        return PausedWaitWake::ControlChanged;
    }
    return PausedWaitWake::Failed;
}

}

FramePacingWaitResult waitForFrameOpportunity(
    const HANDLE frameLatencyWaitable,
    const HANDLE deviceRemovedWaitable,
    const DWORD timeoutMilliseconds) noexcept
{
    const bool deviceNotificationAvailable = deviceRemovedWaitable != nullptr;
    std::array<FramePacingWaitable, 2U> waitables{};
    std::size_t waitableCount = 0U;
    if (deviceNotificationAvailable)
    {
        // Device removal is terminal for the current resource domain. Put it
        // first so a simultaneously available frame cannot delay recovery.
        waitables[waitableCount++] = FramePacingWaitable{
            deviceRemovedWaitable,
            FramePacingWaitableKind::DeviceRemoved,
            0U};
    }
    waitables[waitableCount++] = FramePacingWaitable{
        frameLatencyWaitable,
        FramePacingWaitableKind::FrameReady,
        0U};
    return waitForAnyFrameOpportunity(
        std::span<const FramePacingWaitable>(waitables.data(), waitableCount),
        timeoutMilliseconds);
}

FramePacingWaitResult waitForAnyFrameOpportunity(
    const std::span<const FramePacingWaitable> waitables,
    const DWORD timeoutMilliseconds) noexcept
{
    constexpr std::size_t maximumWaitables = MAXIMUM_WAIT_OBJECTS - 1U;
    std::array<HANDLE, maximumWaitables> handles{};
    for (std::size_t index = 0U; index < waitables.size(); ++index)
    {
        const FramePacingWaitable& waitable = waitables[index];
        if (waitable.handle == nullptr
            || waitable.handle == INVALID_HANDLE_VALUE)
        {
            return FramePacingWaitResult{
                FramePacingWake::Failed,
                ERROR_INVALID_HANDLE,
                waitable.token};
        }
        const DWORD state = WaitForSingleObject(waitable.handle, 0U);
        if (state == WAIT_OBJECT_0)
        {
            return FramePacingWaitResult{
                framePacingWake(waitable.kind),
                ERROR_SUCCESS,
                waitable.token};
        }
        if (state == WAIT_FAILED)
        {
            const DWORD error = GetLastError();
            return FramePacingWaitResult{
                FramePacingWake::Failed,
                error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : error,
                waitable.token};
        }
        if (index < maximumWaitables)
        {
            handles[index] = waitable.handle;
        }
    }

    // Win32 can block on at most MAXIMUM_WAIT_OBJECTS minus the message queue.
    // Handles beyond this window were still polled above and are observed on
    // the next bounded control cycle instead of terminating a large topology.
    const std::size_t blockingCount = (std::min)(
        waitables.size(),
        maximumWaitables);
    SetLastError(ERROR_SUCCESS);
    const DWORD count = static_cast<DWORD>(blockingCount);
    const DWORD result = MsgWaitForMultipleObjectsEx(
        count,
        count > 0U ? handles.data() : nullptr,
        timeoutMilliseconds,
        QS_ALLINPUT,
        MWMO_INPUTAVAILABLE);
    if (result >= WAIT_OBJECT_0 && result < WAIT_OBJECT_0 + count)
    {
        const std::size_t index = static_cast<std::size_t>(
            result - WAIT_OBJECT_0);
        return FramePacingWaitResult{
            framePacingWake(waitables[index].kind),
            ERROR_SUCCESS,
            waitables[index].token};
    }
    if (result == WAIT_OBJECT_0 + count)
    {
        return FramePacingWaitResult{FramePacingWake::MessagesPending};
    }
    if (result == WAIT_TIMEOUT)
    {
        return FramePacingWaitResult{FramePacingWake::TimedOut};
    }

    // Preserve the wait failure at its source. Diagnostics recorded before the
    // caller handles it are allowed to invoke Win32 and change GetLastError().
    const DWORD error = GetLastError();
    return FramePacingWaitResult{
        FramePacingWake::Failed,
        error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : error};
}

HANDLE createFrameCadenceWaitableTimer() noexcept
{
    // CREATE_WAITABLE_TIMER_HIGH_RESOLUTION is intentionally kept as a local
    // ABI value so the same full binary can be built with an older Windows SDK.
    constexpr DWORD highResolutionFlag = 0x00000002U;
    HANDLE timer = CreateWaitableTimerExW(
        nullptr,
        nullptr,
        highResolutionFlag,
        TIMER_MODIFY_STATE | SYNCHRONIZE);
    if (timer != nullptr)
    {
        return timer;
    }

    // Older Windows builds reject the high-resolution flag. A normal waitable
    // timer retains correct bounded behavior, with only coarser wake precision.
    return CreateWaitableTimerExW(
        nullptr,
        nullptr,
        0U,
        TIMER_MODIFY_STATE | SYNCHRONIZE);
}

DWORD armFrameCadenceWaitableTimer(
    const HANDLE timer,
    const std::chrono::nanoseconds delay) noexcept
{
    if (timer == nullptr || timer == INVALID_HANDLE_VALUE)
    {
        return ERROR_INVALID_HANDLE;
    }

    constexpr std::int64_t nanosecondsPerTick = 100LL;
    const std::int64_t nanoseconds = (std::max)(
        delay.count(),
        std::int64_t{1});
    const std::int64_t maximumNanoseconds =
        (std::numeric_limits<std::int64_t>::max)()
        - (nanosecondsPerTick - 1LL);
    const std::int64_t boundedNanoseconds = (std::min)(
        nanoseconds,
        maximumNanoseconds);
    const std::int64_t ticks =
        (boundedNanoseconds + nanosecondsPerTick - 1LL)
        / nanosecondsPerTick;
    LARGE_INTEGER dueTime{};
    dueTime.QuadPart = -ticks;
    SetLastError(ERROR_SUCCESS);
    if (SetWaitableTimer(
            timer,
            &dueTime,
            0,
            nullptr,
            nullptr,
            FALSE))
    {
        return ERROR_SUCCESS;
    }
    const DWORD error = GetLastError();
    return error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : error;
}

PausedWaitResult waitForPausedInvalidation(
    const HANDLE deviceRemovedWaitable,
    const HANDLE backgroundFrameWaitable,
    const DWORD timeoutMilliseconds) noexcept
{
    const bool deviceNotificationAvailable = deviceRemovedWaitable != nullptr;
    const bool backgroundFrameAvailable = backgroundFrameWaitable != nullptr;
    std::array<PausedWaitable, 2U> waitables{};
    std::size_t waitableCount = 0U;
    if (deviceNotificationAvailable)
    {
        // The device event is terminal for both the retained frame and WGC.
        // Keep it first when both sources become ready at once.
        waitables[waitableCount++] = PausedWaitable{
            deviceRemovedWaitable,
            PausedWaitableKind::DeviceRemoved,
            0U};
    }
    if (backgroundFrameAvailable)
    {
        waitables[waitableCount++] = PausedWaitable{
            backgroundFrameWaitable,
            PausedWaitableKind::BackgroundFrameReady,
            0U};
    }

    return waitForAnyPausedInvalidation(
        std::span<const PausedWaitable>(waitables.data(), waitableCount),
        timeoutMilliseconds);
}

PausedWaitResult waitForAnyPausedInvalidation(
    const std::span<const PausedWaitable> waitables,
    const DWORD timeoutMilliseconds) noexcept
{
    constexpr std::size_t maximumWaitables = MAXIMUM_WAIT_OBJECTS - 1U;
    std::array<HANDLE, maximumWaitables> handles{};
    for (std::size_t index = 0U; index < waitables.size(); ++index)
    {
        const PausedWaitable& waitable = waitables[index];
        if (waitable.handle == nullptr
            || waitable.handle == INVALID_HANDLE_VALUE)
        {
            return PausedWaitResult{
                PausedWaitWake::Failed,
                ERROR_INVALID_HANDLE,
                waitable.token};
        }
        const DWORD state = WaitForSingleObject(waitable.handle, 0U);
        if (state == WAIT_OBJECT_0)
        {
            return PausedWaitResult{
                pausedWaitWake(waitable.kind),
                ERROR_SUCCESS,
                waitable.token};
        }
        if (state == WAIT_FAILED)
        {
            const DWORD error = GetLastError();
            return PausedWaitResult{
                PausedWaitWake::Failed,
                error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : error,
                waitable.token};
        }
        if (index < maximumWaitables)
        {
            handles[index] = waitable.handle;
        }
    }

    const std::size_t blockingCount = (std::min)(
        waitables.size(),
        maximumWaitables);
    SetLastError(ERROR_SUCCESS);
    const DWORD waitableCount = static_cast<DWORD>(blockingCount);
    const DWORD result = MsgWaitForMultipleObjectsEx(
        waitableCount,
        waitableCount > 0U ? handles.data() : nullptr,
        timeoutMilliseconds,
        QS_ALLINPUT,
        MWMO_INPUTAVAILABLE);
    if (result >= WAIT_OBJECT_0
        && result < WAIT_OBJECT_0 + waitableCount)
    {
        const std::size_t index = static_cast<std::size_t>(
            result - WAIT_OBJECT_0);
        return PausedWaitResult{
            pausedWaitWake(waitables[index].kind),
            ERROR_SUCCESS,
            waitables[index].token};
    }
    if (result == WAIT_OBJECT_0 + waitableCount)
    {
        return PausedWaitResult{PausedWaitWake::MessagesPending};
    }
    if (result == WAIT_TIMEOUT)
    {
        return PausedWaitResult{PausedWaitWake::TimedOut};
    }

    const DWORD error = GetLastError();
    return PausedWaitResult{
        PausedWaitWake::Failed,
        error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : error};
}

}
