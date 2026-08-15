#include "test_support.hpp"

#include "bafx/windows/detail/wgc_idle_drain_policy.hpp"

#include <chrono>

using bafx::windows::detail::WgcDrainPolicyAction;
using bafx::windows::detail::WgcDrainPolicyState;

namespace
{

using namespace std::chrono_literals;

}

BAFX_TEST(wgc_idle_drain_policy_attempts_at_the_fifty_millisecond_boundary)
{
    const auto first = bafx::windows::detail::decideWgcDrain(
        false,
        7U,
        100ms,
        WgcDrainPolicyState{});
    BAFX_CHECK(first.action == WgcDrainPolicyAction::IdleAttempt);

    const auto beforeBoundary = bafx::windows::detail::decideWgcDrain(
        false,
        7U,
        149ms + 999us,
        first.nextState);
    BAFX_CHECK(beforeBoundary.action == WgcDrainPolicyAction::IdleThrottled);
    BAFX_CHECK(beforeBoundary.nextState.lastAttemptAt == 100ms);

    const auto atBoundary = bafx::windows::detail::decideWgcDrain(
        false,
        7U,
        150ms,
        beforeBoundary.nextState);
    BAFX_CHECK(atBoundary.action == WgcDrainPolicyAction::IdleAttempt);
    BAFX_CHECK(atBoundary.nextState.lastAttemptAt == 150ms);
}

BAFX_TEST(wgc_idle_drain_policy_never_throttles_visible_content)
{
    const WgcDrainPolicyState previous{7U, 100ms, true};
    const auto decision = bafx::windows::detail::decideWgcDrain(
        true,
        7U,
        101ms,
        previous);

    BAFX_CHECK(decision.action == WgcDrainPolicyAction::VisibleAttempt);
    BAFX_CHECK(decision.nextState.lastAttemptAt == 101ms);
}

BAFX_TEST(wgc_idle_drain_policy_recovers_from_clock_regression)
{
    const WgcDrainPolicyState previous{7U, 100ms, true};
    const auto regressed = bafx::windows::detail::decideWgcDrain(
        false,
        7U,
        90ms,
        previous);
    BAFX_CHECK(regressed.action == WgcDrainPolicyAction::IdleAttempt);
    BAFX_CHECK(regressed.nextState.lastAttemptAt == 90ms);

    const auto stabilized = bafx::windows::detail::decideWgcDrain(
        false,
        7U,
        91ms,
        regressed.nextState);
    BAFX_CHECK(stabilized.action == WgcDrainPolicyAction::IdleThrottled);
}

BAFX_TEST(wgc_idle_drain_policy_attempts_immediately_for_a_new_session)
{
    const WgcDrainPolicyState previous{7U, 100ms, true};
    const auto decision = bafx::windows::detail::decideWgcDrain(
        false,
        8U,
        101ms,
        previous);

    BAFX_CHECK(decision.action == WgcDrainPolicyAction::IdleAttempt);
    BAFX_CHECK(decision.nextState.epoch == 8U);
    BAFX_CHECK(decision.nextState.lastAttemptAt == 101ms);
}

BAFX_TEST(wgc_idle_drain_policy_handles_duration_extremes_without_subtraction)
{
    const auto elapsed = bafx::windows::detail::decideWgcDrain(
        false,
        7U,
        bafx::core::MonotonicTime::max(),
        WgcDrainPolicyState{
            7U,
            bafx::core::MonotonicTime::min(),
            true});
    BAFX_CHECK(elapsed.action == WgcDrainPolicyAction::IdleAttempt);

    const bafx::core::MonotonicTime beforeBoundary =
        bafx::core::MonotonicTime::min()
        + bafx::windows::detail::wgcIdleDrainInterval
        - 1ns;
    const auto throttled = bafx::windows::detail::decideWgcDrain(
        false,
        7U,
        beforeBoundary,
        WgcDrainPolicyState{
            7U,
            bafx::core::MonotonicTime::min(),
            true});
    BAFX_CHECK(throttled.action == WgcDrainPolicyAction::IdleThrottled);
}
