#pragma once

#include <windows.h>

#include <cstdint>

namespace bafx::desktop
{

enum class FramePacingWake : std::uint8_t
{
    FrameReady,
    MessagesPending,
    TimedOut,
    Failed
};

// Raw Input must wake the message pump without granting another GPU submission.
// Only the swap-chain latency object represents an available presentation slot.
[[nodiscard]] FramePacingWake waitForFrameOpportunity(
    HANDLE frameLatencyWaitable,
    DWORD timeoutMilliseconds) noexcept;

}
