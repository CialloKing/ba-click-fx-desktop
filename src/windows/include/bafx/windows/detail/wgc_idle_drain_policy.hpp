#pragma once

#include "bafx/core/background_freshness.hpp"

#include <chrono>
#include <cstdint>

namespace bafx::windows::detail
{

inline constexpr bafx::core::MonotonicTime wgcIdleDrainInterval =
    std::chrono::milliseconds(50);

enum class WgcDrainPolicyAction : std::uint8_t
{
    VisibleAttempt,
    IdleAttempt,
    IdleThrottled
};

struct WgcDrainPolicyState
{
    std::uint64_t epoch{0U};
    bafx::core::MonotonicTime lastAttemptAt{};
    bool initialized{false};
};

struct WgcDrainPolicyDecision
{
    WgcDrainPolicyAction action{WgcDrainPolicyAction::IdleThrottled};
    WgcDrainPolicyState nextState{};
};

// The transition is pure so session changes and non-monotonic test clocks can
// be verified without constructing WGC or a D3D immediate context.
[[nodiscard]] WgcDrainPolicyDecision decideWgcDrain(
    bool hasDrawableContent,
    std::uint64_t epoch,
    bafx::core::MonotonicTime now,
    const WgcDrainPolicyState& state) noexcept;

}
