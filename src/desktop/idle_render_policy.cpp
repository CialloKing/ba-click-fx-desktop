#include "idle_render_policy.hpp"

namespace bafx::desktop
{

bool shouldRenderForIdlePolicy(
    const IdleRenderPolicyInput& input) noexcept
{
    return evaluateIdleRenderPolicy(input).shouldRender;
}

IdleRenderDecision evaluateIdleRenderPolicy(
    const IdleRenderPolicyInput& input) noexcept
{
    if (input.displayPowerUnavailable && !input.independentOutputRequired)
    {
        return {false, "display-power-unavailable"};
    }
    if (input.paused)
    {
        return {input.enteringPause || input.renderInvalidated,
            input.enteringPause || input.renderInvalidated ? "pause-cleanup" : "paused"};
    }
    if (!input.idleOptimizationEnabled
        || input.continuousRenderingRequired)
    {
        return {true, "continuous-rendering"};
    }
    if (input.renderInvalidated)
    {
        return {true, "render-invalidated"};
    }
    if (input.pointerInputPending)
    {
        return {true, "pointer-input-pending"};
    }
    if (input.activeEffects)
    {
        return {true, "active-effects"};
    }
    if (input.presentedDrawableContent)
    {
        return {true, "clear-last-frame"};
    }
    return {false, "idle-no-content"};
}

}
