#include "frame_pacing.hpp"

namespace bafx::desktop
{

FramePacingWaitResult waitForFrameOpportunity(
    const HANDLE frameLatencyWaitable,
    const DWORD timeoutMilliseconds) noexcept
{
    if (frameLatencyWaitable == nullptr
        || frameLatencyWaitable == INVALID_HANDLE_VALUE)
    {
        return FramePacingWaitResult{
            FramePacingWake::Failed,
            ERROR_INVALID_HANDLE};
    }

    const DWORD result = MsgWaitForMultipleObjectsEx(
        1U,
        &frameLatencyWaitable,
        timeoutMilliseconds,
        QS_ALLINPUT,
        MWMO_INPUTAVAILABLE);
    if (result == WAIT_OBJECT_0)
    {
        return FramePacingWaitResult{FramePacingWake::FrameReady};
    }
    if (result == WAIT_OBJECT_0 + 1U)
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
