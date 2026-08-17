#pragma once

namespace bafx::desktop
{

struct IdleRenderPolicyInput final
{
    bool displayPowerUnavailable{false};
    bool paused{false};
    bool enteringPause{false};
    bool renderInvalidated{false};
    bool idleOptimizationEnabled{true};
    bool continuousRenderingRequired{false};
    bool pointerInputPending{false};
    bool activeEffects{false};
    bool presentedDrawableContent{false};
};

[[nodiscard]] bool shouldRenderForIdlePolicy(
    const IdleRenderPolicyInput& input) noexcept;

}
