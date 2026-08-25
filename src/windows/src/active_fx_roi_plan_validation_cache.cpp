#include "bafx/windows/detail/active_fx_roi_plan_validation_cache.hpp"

#include <cstddef>

namespace bafx::windows::detail
{
namespace
{

[[nodiscard]] bool sameRect(
    const bafx::core::RectI left,
    const bafx::core::RectI right) noexcept
{
    return left.left == right.left
        && left.top == right.top
        && left.right == right.right
        && left.bottom == right.bottom;
}

[[nodiscard]] bool samePixelTotals(
    const bafx::core::UnityBloomPassPixelTotals left,
    const bafx::core::UnityBloomPassPixelTotals right) noexcept
{
    return left.fullPixels == right.fullPixels
        && left.candidatePixels == right.candidatePixels;
}

[[nodiscard]] bool sameBloomPlan(
    const bafx::core::UnityBloomPlan& left,
    const bafx::core::UnityBloomPlan& right) noexcept
{
    if (left.mipCount != right.mipCount
        || left.sampleScale != right.sampleScale
        || left.exposureGain != right.exposureGain)
    {
        return false;
    }
    for (std::size_t index = 0U;
         index < bafx::core::unityBloomMaxMipCount;
         ++index)
    {
        if (left.mipChain[index].width != right.mipChain[index].width
            || left.mipChain[index].height != right.mipChain[index].height)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool samePassPlan(
    const bafx::core::UnityBloomPassRoiPlan& left,
    const bafx::core::UnityBloomPassRoiPlan& right) noexcept
{
    if (left.mipCount != right.mipCount
        || left.basePlan.guardX != right.basePlan.guardX
        || left.basePlan.guardY != right.basePlan.guardY
        || left.basePlan.phasePeriod != right.basePlan.phasePeriod
        || !sameRect(left.basePlan.sourceSupport, right.basePlan.sourceSupport)
        || !sameRect(left.basePlan.bloomOutput, right.basePlan.bloomOutput)
        || !sameRect(left.basePlan.alignedWork, right.basePlan.alignedWork)
        || !sameRect(left.resolveRect, right.resolveRect)
        || !samePixelTotals(left.prefilterPixels, right.prefilterPixels)
        || !samePixelTotals(left.pyramidPixels, right.pyramidPixels)
        || !samePixelTotals(left.resolvePixels, right.resolvePixels)
        || !samePixelTotals(left.totalPixels, right.totalPixels))
    {
        return false;
    }
    for (std::size_t index = 0U; index < left.mipCount; ++index)
    {
        if (!sameRect(left.downRects[index], right.downRects[index]))
        {
            return false;
        }
        if (index + 1U < left.mipCount
            && !sameRect(left.upRects[index], right.upRects[index]))
        {
            return false;
        }
    }
    return true;
}

}

ActiveFxRoiPlanValidationResult ActiveFxRoiPlanValidationCache::validate(
    const bafx::core::UnityBloomPassRoiPlan& candidate,
    const bafx::core::RectI monitorBounds,
    const bafx::core::UnityBloomPlan& bloomPlan) noexcept
{
    if (entry_.has_value()
        && sameRect(entry_->monitorBounds, monitorBounds)
        && sameBloomPlan(entry_->bloomPlan, bloomPlan)
        && samePassPlan(candidate, entry_->expected))
    {
        return ActiveFxRoiPlanValidationResult{true, true};
    }

    const bafx::core::UnityBloomPassRoiPlanResult expected =
        bafx::core::planUnityBloomPassRoi(
            candidate.basePlan.sourceSupport,
            monitorBounds,
            bloomPlan);
    if (expected.status != bafx::core::RoiStatus::Ok
        || !samePassPlan(candidate, expected.plan))
    {
        return ActiveFxRoiPlanValidationResult{};
    }

    // Never retain caller-owned data: only the planner's independently
    // reconstructed result is eligible to authorize a later cache hit.
    entry_ = Entry{monitorBounds, bloomPlan, expected.plan};
    return ActiveFxRoiPlanValidationResult{true, false};
}

void ActiveFxRoiPlanValidationCache::reset() noexcept
{
    entry_.reset();
}

}
