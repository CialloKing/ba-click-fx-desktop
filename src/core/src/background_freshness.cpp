#include "bafx/core/background_freshness.hpp"

#include <limits>

namespace bafx::core
{
namespace
{

[[nodiscard]] MonotonicTime saturatingDifference(
    const MonotonicTime left,
    const MonotonicTime right) noexcept
{
    using Rep = MonotonicTime::rep;
    constexpr Rep minimum = std::numeric_limits<Rep>::min();
    constexpr Rep maximum = std::numeric_limits<Rep>::max();
    const Rep leftCount = left.count();
    const Rep rightCount = right.count();

    // Driver timestamps are outside our trust boundary. Saturation preserves
    // their direction without invoking signed overflow in duration subtraction.
    if (rightCount > 0 && leftCount < minimum + rightCount)
    {
        return MonotonicTime::min();
    }
    if (rightCount < 0 && leftCount > maximum + rightCount)
    {
        return MonotonicTime::max();
    }
    return MonotonicTime{leftCount - rightCount};
}

}

BackgroundRenderPath BackgroundPathLatch::select(
    const bool hasVisibleContent,
    const bool acquireAllowed,
    const bool retainAllowed) noexcept
{
    if (!hasVisibleContent)
    {
        reset();
        return BackgroundRenderPath::FxOnly;
    }

    if (!path_.has_value())
    {
        path_ = acquireAllowed
            ? BackgroundRenderPath::BackgroundAware
            : BackgroundRenderPath::FxOnly;
    }
    else if (*path_ == BackgroundRenderPath::BackgroundAware
        && !retainAllowed)
    {
        // Downgrade only while the renderer has not captured a stable batch
        // snapshot yet. Once a snapshot exists, the caller keeps retainAllowed
        // true even when the live WGC sample ages out, so the visible batch
        // cannot flash between background-aware and FX-only composition.
        path_ = BackgroundRenderPath::FxOnly;
    }

    return *path_;
}

void BackgroundPathLatch::reset() noexcept
{
    path_.reset();
}

BackgroundUsageDecision evaluateBackgroundUsage(
    const std::optional<BackgroundFrameStamp>& frame,
    const MonotonicTime renderAt,
    const BackgroundUsagePolicy& policy) noexcept
{
    BackgroundUsageDecision decision{};
    if (policy.maxAge <= MonotonicTime::zero()
        || policy.maxFutureSkew < MonotonicTime::zero())
    {
        decision.status = BackgroundUsageStatus::InvalidPolicy;
        return decision;
    }

    if (!frame.has_value())
    {
        decision.status = BackgroundUsageStatus::Missing;
        return decision;
    }

    if (frame->epoch != policy.expectedEpoch)
    {
        decision.status = BackgroundUsageStatus::WrongEpoch;
        return decision;
    }

    if (!frame->canonicalLinearScRgb || !frame->excludesOwnOverlay)
    {
        decision.status = BackgroundUsageStatus::InvalidContract;
        return decision;
    }

    decision.age = saturatingDifference(renderAt, frame->capturedAt);
    if (decision.age < -policy.maxFutureSkew)
    {
        decision.status = BackgroundUsageStatus::FutureTimestamp;
        return decision;
    }
    if (decision.age >= policy.maxAge)
    {
        decision.status = BackgroundUsageStatus::Stale;
        return decision;
    }

    decision.status = BackgroundUsageStatus::Usable;
    decision.enabled = true;
    return decision;
}

}
