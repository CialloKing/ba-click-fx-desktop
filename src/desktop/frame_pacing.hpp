#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <span>

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
    std::size_t token{0U};
};

enum class FramePacingWaitableKind : std::uint8_t
{
    FrameReady,
    DeviceRemoved
};

struct FramePacingWaitable final
{
    HANDLE handle{nullptr};
    FramePacingWaitableKind kind{FramePacingWaitableKind::FrameReady};
    std::size_t token{0U};
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
    std::size_t token{0U};
};

enum class PausedWaitableKind : std::uint8_t
{
    DeviceRemoved,
    BackgroundFrameReady
};

struct PausedWaitable final
{
    HANDLE handle{nullptr};
    PausedWaitableKind kind{PausedWaitableKind::DeviceRemoved};
    std::size_t token{0U};
};

// Raw Input must wake the message pump without granting another GPU submission.
// Only the swap-chain latency object represents an available presentation slot.
[[nodiscard]] FramePacingWaitResult waitForFrameOpportunity(
    HANDLE frameLatencyWaitable,
    HANDLE deviceRemovedWaitable,
    DWORD timeoutMilliseconds) noexcept;

// Mixed-refresh sessions contribute independent latency and device events.
// Every handle is polled; at most the Win32 limit is included in the blocking
// wait, so unusually large virtual-display topologies remain bounded.
// The caller owns token interpretation and renders only the granted session.
[[nodiscard]] FramePacingWaitResult waitForAnyFrameOpportunity(
    std::span<const FramePacingWaitable> waitables,
    DWORD timeoutMilliseconds) noexcept;

// A paused Host does not submit frames, but device removal must still wake it
// so the existing one-shot recovery boundary can rebuild the resource domain.
[[nodiscard]] PausedWaitResult waitForPausedInvalidation(
    HANDLE deviceRemovedWaitable,
    HANDLE backgroundFrameWaitable,
    DWORD timeoutMilliseconds) noexcept;

[[nodiscard]] PausedWaitResult waitForAnyPausedInvalidation(
    std::span<const PausedWaitable> waitables,
    DWORD timeoutMilliseconds) noexcept;

}
