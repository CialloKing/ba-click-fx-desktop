#pragma once

#include <windows.h>

#include <cstdint>

namespace bafx::desktop
{

enum class FramePacingWake : std::uint8_t
{
    FrameReady,
    DeviceRemoved,
    MessagesPending,
    TimedOut,
    Failed
};

struct FramePacingWaitResult
{
    FramePacingWake wake{FramePacingWake::Failed};
    DWORD error{ERROR_SUCCESS};
};

// Raw Input must wake the message pump without granting another GPU submission.
// Only the swap-chain latency object represents an available presentation slot.
[[nodiscard]] FramePacingWaitResult waitForFrameOpportunity(
    HANDLE frameLatencyWaitable,
    HANDLE deviceRemovedWaitable,
    DWORD timeoutMilliseconds) noexcept;

}
