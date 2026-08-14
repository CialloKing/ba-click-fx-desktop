#include "frame_pacing.hpp"

namespace bafx::desktop
{

FramePacingWake waitForFrameOpportunity(
    const HANDLE frameLatencyWaitable,
    const DWORD timeoutMilliseconds) noexcept
{
    if (frameLatencyWaitable == nullptr
        || frameLatencyWaitable == INVALID_HANDLE_VALUE)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FramePacingWake::Failed;
    }

    const DWORD result = MsgWaitForMultipleObjectsEx(
        1U,
        &frameLatencyWaitable,
        timeoutMilliseconds,
        QS_ALLINPUT,
        MWMO_INPUTAVAILABLE);
    if (result == WAIT_OBJECT_0)
    {
        return FramePacingWake::FrameReady;
    }
    if (result == WAIT_OBJECT_0 + 1U)
    {
        return FramePacingWake::MessagesPending;
    }
    if (result == WAIT_TIMEOUT)
    {
        return FramePacingWake::TimedOut;
    }
    return FramePacingWake::Failed;
}

}
