#include "bafx/windows/detail/wgc_idle_drain_policy.hpp"

namespace bafx::windows::detail
{
namespace
{

[[nodiscard]] bool intervalElapsedWithoutOverflow(
    const bafx::core::MonotonicTime now,
    const bafx::core::MonotonicTime previous,
    const bafx::core::MonotonicTime interval) noexcept
{
    if (interval <= bafx::core::MonotonicTime::zero()
        || now < previous)
    {
        return false;
    }
    if (now < bafx::core::MonotonicTime::min() + interval)
    {
        // No representable previous value can be one full interval behind.
        return false;
    }
    return previous <= now - interval;
}

}

WgcDrainPolicyDecision decideWgcDrain(
    const bool hasDrawableContent,
    const std::uint64_t epoch,
    const bafx::core::MonotonicTime now,
    const WgcDrainPolicyState& state) noexcept
{
    const bool sessionChanged = !state.initialized || state.epoch != epoch;
    const bool clockRegressed = state.initialized
        && state.epoch == epoch
        && now < state.lastAttemptAt;
    const bool idleIntervalElapsed = state.initialized
        && state.epoch == epoch
        && intervalElapsedWithoutOverflow(
            now,
            state.lastAttemptAt,
            wgcIdleDrainInterval);
    const bool shouldAttempt = hasDrawableContent
        || sessionChanged
        || clockRegressed
        || idleIntervalElapsed;
    if (!shouldAttempt)
    {
        return WgcDrainPolicyDecision{
            WgcDrainPolicyAction::IdleThrottled,
            state};
    }

    return WgcDrainPolicyDecision{
        hasDrawableContent
            ? WgcDrainPolicyAction::VisibleAttempt
            : WgcDrainPolicyAction::IdleAttempt,
        WgcDrainPolicyState{epoch, now, true}};
}

}
