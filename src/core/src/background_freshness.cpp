#include "bafx/core/background_freshness.hpp"

#include <algorithm>

namespace bafx::core
{

BackgroundFreshnessResult evaluateBackgroundFreshness(
    const std::optional<BackgroundFrameStamp>& frame,
    const MonotonicTime renderAt,
    const BackgroundFreshnessPolicy& policy) noexcept
{
    BackgroundFreshnessResult result{};

    if (policy.fullWeightAge < MonotonicTime::zero()
        || policy.staleAge <= policy.fullWeightAge
        || policy.maxFutureSkew < MonotonicTime::zero())
    {
        result.freshness = BackgroundFreshness::InvalidPolicy;
        return result;
    }

    if (!frame.has_value())
    {
        result.freshness = BackgroundFreshness::Missing;
        return result;
    }

    if (frame->epoch != policy.expectedEpoch)
    {
        result.freshness = BackgroundFreshness::WrongEpoch;
        return result;
    }

    if (!frame->canonicalLinearScRgb || !frame->excludesOwnOverlay)
    {
        result.freshness = BackgroundFreshness::InvalidContract;
        return result;
    }

    result.age = renderAt - frame->capturedAt;
    if (result.age < -policy.maxFutureSkew)
    {
        result.freshness = BackgroundFreshness::FutureTimestamp;
        return result;
    }

    if (result.age <= policy.fullWeightAge)
    {
        result.freshness = BackgroundFreshness::Fresh;
        result.weight = 1.0F;
        return result;
    }

    if (result.age >= policy.staleAge)
    {
        result.freshness = BackgroundFreshness::Stale;
        return result;
    }

    const auto fadeDuration = policy.staleAge - policy.fullWeightAge;
    const auto fadeAge = result.age - policy.fullWeightAge;
    const auto fadeFraction = static_cast<double>(fadeAge.count())
        / static_cast<double>(fadeDuration.count());

    result.freshness = BackgroundFreshness::Fading;
    result.weight = std::clamp(static_cast<float>(1.0 - fadeFraction), 0.0F, 1.0F);
    return result;
}

BackgroundUsageDecision evaluateBackgroundUsage(
    const std::optional<BackgroundFrameStamp>& frame,
    const MonotonicTime renderAt,
    const BackgroundUsagePolicy& policy) noexcept
{
    BackgroundUsageDecision decision{};
    if (policy.transportStaleAge < policy.differentialBloom.staleAge
        || policy.transportMaxFutureSkew
            < policy.differentialBloom.maxFutureSkew)
    {
        decision.freshness.freshness = BackgroundFreshness::InvalidPolicy;
        return decision;
    }

    decision.freshness = evaluateBackgroundFreshness(
        frame,
        renderAt,
        policy.differentialBloom);
    switch (decision.freshness.freshness)
    {
    case BackgroundFreshness::Fresh:
    case BackgroundFreshness::Fading:
        decision.transportEnabled = true;
        break;
    case BackgroundFreshness::Stale:
        decision.transportEnabled = decision.freshness.age
            < policy.transportStaleAge;
        break;
    case BackgroundFreshness::FutureTimestamp:
        decision.transportEnabled = decision.freshness.age
            >= -policy.transportMaxFutureSkew;
        break;
    case BackgroundFreshness::Missing:
    case BackgroundFreshness::WrongEpoch:
    case BackgroundFreshness::InvalidContract:
    case BackgroundFreshness::InvalidPolicy:
        break;
    }
    return decision;
}

BloomInputMode selectBloomInputMode(
    const BackgroundFreshnessResult& freshness,
    const BackgroundFallback fallback) noexcept
{
    if ((freshness.freshness == BackgroundFreshness::Fresh
            || freshness.freshness == BackgroundFreshness::Fading)
        && freshness.weight > 0.0F)
    {
        return BloomInputMode::BackgroundAware;
    }

    if (fallback == BackgroundFallback::FxOnlyBloom)
    {
        return BloomInputMode::FxOnly;
    }

    return BloomInputMode::Disabled;
}

}
