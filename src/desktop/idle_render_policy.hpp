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
    // Independent outputs such as Spout2 must keep producing frames even
    // when the monitor power notification suspends the visible composition.
    bool independentOutputRequired{false};
};

[[nodiscard]] bool shouldRenderForIdlePolicy(
    const IdleRenderPolicyInput& input) noexcept;

}
