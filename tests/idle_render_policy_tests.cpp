#include "test_support.hpp"

#include "idle_render_policy.hpp"

using bafx::desktop::IdleRenderPolicyInput;
using bafx::desktop::shouldRenderForIdlePolicy;

BAFX_TEST(idle_render_policy_parks_an_empty_product_frame)
{
    BAFX_CHECK(!shouldRenderForIdlePolicy(IdleRenderPolicyInput{}));
}

BAFX_TEST(idle_render_policy_wakes_for_input_effects_and_clear_frames)
{
    IdleRenderPolicyInput input{};
    input.pointerInputPending = true;
    BAFX_CHECK(shouldRenderForIdlePolicy(input));

    input = IdleRenderPolicyInput{};
    input.activeEffects = true;
    BAFX_CHECK(shouldRenderForIdlePolicy(input));

    input = IdleRenderPolicyInput{};
    input.presentedDrawableContent = true;
    BAFX_CHECK(shouldRenderForIdlePolicy(input));

    input = IdleRenderPolicyInput{};
    input.renderInvalidated = true;
    BAFX_CHECK(shouldRenderForIdlePolicy(input));
}

BAFX_TEST(idle_render_policy_preserves_continuous_and_pause_contracts)
{
    IdleRenderPolicyInput input{};
    input.idleOptimizationEnabled = false;
    BAFX_CHECK(shouldRenderForIdlePolicy(input));

    input = IdleRenderPolicyInput{};
    input.continuousRenderingRequired = true;
    BAFX_CHECK(shouldRenderForIdlePolicy(input));

    input = IdleRenderPolicyInput{};
    input.paused = true;
    BAFX_CHECK(!shouldRenderForIdlePolicy(input));
    input.enteringPause = true;
    BAFX_CHECK(shouldRenderForIdlePolicy(input));

    input.displayPowerUnavailable = true;
    BAFX_CHECK(!shouldRenderForIdlePolicy(input));

    input.independentOutputRequired = true;
    BAFX_CHECK(shouldRenderForIdlePolicy(input));
}

BAFX_TEST(idle_render_policy_parks_an_empty_independent_output)
{
    IdleRenderPolicyInput input{};
    input.independentOutputRequired = true;
    BAFX_CHECK(!shouldRenderForIdlePolicy(input));

    input.displayPowerUnavailable = true;
    BAFX_CHECK(!shouldRenderForIdlePolicy(input));

    input.activeEffects = true;
    BAFX_CHECK(shouldRenderForIdlePolicy(input));

    input.activeEffects = false;
    input.presentedDrawableContent = true;
    BAFX_CHECK(shouldRenderForIdlePolicy(input));
}
