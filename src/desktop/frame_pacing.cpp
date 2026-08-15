#include "frame_pacing.hpp"

#include <array>

namespace bafx::desktop
{

FramePacingWaitResult waitForFrameOpportunity(
    const HANDLE frameLatencyWaitable,
    const HANDLE deviceRemovedWaitable,
    const DWORD timeoutMilliseconds) noexcept
{
    if (frameLatencyWaitable == nullptr
        || frameLatencyWaitable == INVALID_HANDLE_VALUE)
    {
        return FramePacingWaitResult{
            FramePacingWake::Failed,
            ERROR_INVALID_HANDLE};
    }

    if (deviceRemovedWaitable == INVALID_HANDLE_VALUE)
    {
        return FramePacingWaitResult{
            FramePacingWake::Failed,
            ERROR_INVALID_HANDLE};
    }

    const bool deviceNotificationAvailable = deviceRemovedWaitable != nullptr;
    std::array<HANDLE, 2U> waitables{};
    DWORD waitableCount = 1U;
    if (deviceNotificationAvailable)
    {
        // Device removal is terminal for the current resource domain. Put it
        // first so a simultaneously available frame cannot delay recovery.
        waitables[0] = deviceRemovedWaitable;
        waitables[1] = frameLatencyWaitable;
        waitableCount = 2U;
    }
    else
    {
        waitables[0] = frameLatencyWaitable;
    }
    SetLastError(ERROR_SUCCESS);
    const DWORD result = MsgWaitForMultipleObjectsEx(
        waitableCount,
        waitables.data(),
        timeoutMilliseconds,
        QS_ALLINPUT,
        MWMO_INPUTAVAILABLE);
    if (result == WAIT_OBJECT_0)
    {
        return FramePacingWaitResult{
            deviceNotificationAvailable
                ? FramePacingWake::DeviceRemoved
                : FramePacingWake::FrameReady};
    }
    if (deviceNotificationAvailable && result == WAIT_OBJECT_0 + 1U)
    {
        return FramePacingWaitResult{FramePacingWake::FrameReady};
    }
    if (result == WAIT_OBJECT_0 + waitableCount)
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

}
