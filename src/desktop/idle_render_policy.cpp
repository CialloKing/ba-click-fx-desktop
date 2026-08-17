#include "idle_render_policy.hpp"

namespace bafx::desktop
{

bool shouldRenderForIdlePolicy(
    const IdleRenderPolicyInput& input) noexcept
{
    if (input.displayPowerUnavailable)
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
    return input.renderInvalidated
        || input.pointerInputPending
        || input.activeEffects
        || input.presentedDrawableContent;
}

}
