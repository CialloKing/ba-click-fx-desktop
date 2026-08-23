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
    // Independent outputs such as Spout2 may render active/clear frames while
    // display power is unavailable. Their separate heartbeat owns idle liveness.
    bool independentOutputRequired{false};
};

[[nodiscard]] bool shouldRenderForIdlePolicy(
    const IdleRenderPolicyInput& input) noexcept;

}
