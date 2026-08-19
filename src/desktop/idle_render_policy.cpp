#include "idle_render_policy.hpp"

namespace bafx::desktop
{

bool shouldRenderForIdlePolicy(
    const IdleRenderPolicyInput& input) noexcept
{
    if (input.displayPowerUnavailable && !input.independentOutputRequired)
    {
        return false;
    }
    if (input.paused)
    {
        return input.enteringPause || input.renderInvalidated;
    }
    if (!input.idleOptimizationEnabled
        || input.continuousRenderingRequired)
    {
        return true;
    }
    if (input.independentOutputRequired)
    {
        return true;
    }
    return input.renderInvalidated
        || input.pointerInputPending
        || input.activeEffects
        || input.presentedDrawableContent;
}

}
