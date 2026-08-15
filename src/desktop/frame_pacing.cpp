#include "frame_pacing.hpp"

#include <array>

namespace bafx::desktop
{

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
    if (waitables.empty() || waitables.size() > maximumWaitables)
    {
        return FramePacingWaitResult{
            FramePacingWake::Failed,
            ERROR_INVALID_PARAMETER};
    }

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
        handles[index] = waitable.handle;
    }

    SetLastError(ERROR_SUCCESS);
    const DWORD count = static_cast<DWORD>(waitables.size());
    const DWORD result = MsgWaitForMultipleObjectsEx(
        count,
        handles.data(),
        timeoutMilliseconds,
        QS_ALLINPUT,
        MWMO_INPUTAVAILABLE);
    if (result >= WAIT_OBJECT_0 && result < WAIT_OBJECT_0 + count)
    {
        const std::size_t index = static_cast<std::size_t>(
            result - WAIT_OBJECT_0);
        return FramePacingWaitResult{
            waitables[index].kind == FramePacingWaitableKind::DeviceRemoved
                ? FramePacingWake::DeviceRemoved
                : FramePacingWake::FrameReady,
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

PausedWaitResult waitForPausedInvalidation(
    const HANDLE deviceRemovedWaitable,
    const HANDLE backgroundFrameWaitable,
    const DWORD timeoutMilliseconds) noexcept
{
    if (deviceRemovedWaitable == INVALID_HANDLE_VALUE
        || backgroundFrameWaitable == INVALID_HANDLE_VALUE)
    {
        return PausedWaitResult{
            PausedWaitWake::Failed,
            ERROR_INVALID_HANDLE};
    }

    const bool deviceNotificationAvailable = deviceRemovedWaitable != nullptr;
    const bool backgroundFrameAvailable = backgroundFrameWaitable != nullptr;
    std::array<HANDLE, 2U> waitables{};
    DWORD waitableCount = 0U;
    if (deviceNotificationAvailable)
    {
        // The device event is terminal for both the retained frame and WGC.
        // Keep it first when both sources become ready at once.
        waitables[waitableCount++] = deviceRemovedWaitable;
    }
    if (backgroundFrameAvailable)
    {
        waitables[waitableCount++] = backgroundFrameWaitable;
    }

    SetLastError(ERROR_SUCCESS);
    const DWORD result = MsgWaitForMultipleObjectsEx(
        waitableCount,
        waitableCount > 0U ? waitables.data() : nullptr,
        timeoutMilliseconds,
        QS_ALLINPUT,
        MWMO_INPUTAVAILABLE);
    if (deviceNotificationAvailable && result == WAIT_OBJECT_0)
    {
        return PausedWaitResult{PausedWaitWake::DeviceRemoved};
    }
    const DWORD backgroundIndex = deviceNotificationAvailable ? 1U : 0U;
    if (backgroundFrameAvailable
        && result == WAIT_OBJECT_0 + backgroundIndex)
    {
        return PausedWaitResult{PausedWaitWake::BackgroundFrameReady};
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
