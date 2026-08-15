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

enum class PausedWaitWake : std::uint8_t
{
    DeviceRemoved,
    BackgroundFrameReady,
    MessagesPending,
    TimedOut,
    Failed
};

struct PausedWaitResult
{
    PausedWaitWake wake{PausedWaitWake::Failed};
    DWORD error{ERROR_SUCCESS};
};

// Raw Input must wake the message pump without granting another GPU submission.
// Only the swap-chain latency object represents an available presentation slot.
[[nodiscard]] FramePacingWaitResult waitForFrameOpportunity(
    HANDLE frameLatencyWaitable,
    HANDLE deviceRemovedWaitable,
    DWORD timeoutMilliseconds) noexcept;

// A paused Host does not submit frames, but device removal must still wake it
// so the existing one-shot recovery boundary can rebuild the resource domain.
[[nodiscard]] PausedWaitResult waitForPausedInvalidation(
    HANDLE deviceRemovedWaitable,
    HANDLE backgroundFrameWaitable,
    DWORD timeoutMilliseconds) noexcept;

}
