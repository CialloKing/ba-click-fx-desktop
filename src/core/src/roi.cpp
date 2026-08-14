#include "bafx/core/roi.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace bafx::core
{
namespace
{

[[nodiscard]] bool isValidRect(const RectI rect) noexcept
{
    return rect.left <= rect.right && rect.top <= rect.bottom;
}

[[nodiscard]] bool isEmpty(const RectI rect) noexcept
{
    return rect.left == rect.right || rect.top == rect.bottom;
}

[[nodiscard]] RectI intersect(const RectI lhs, const RectI rhs) noexcept
{
    return RectI{
        std::max(lhs.left, rhs.left),
        std::max(lhs.top, rhs.top),
        std::min(lhs.right, rhs.right),
        std::min(lhs.bottom, rhs.bottom)};
}

[[nodiscard]] std::int64_t alignDown(
    const std::int64_t value,
    const std::int64_t origin,
    const std::int64_t period) noexcept
{
    const std::int64_t offset = value - origin;
    return origin + (offset / period) * period;
}

[[nodiscard]] std::int64_t alignUp(
    const std::int64_t value,
    const std::int64_t origin,
    const std::int64_t period) noexcept
{
    const std::int64_t offset = value - origin;
    return origin + ((offset + period - 1) / period) * period;
}

[[nodiscard]] bool fitsInt32(const std::int64_t value) noexcept
{
    return value >= std::numeric_limits<std::int32_t>::min()
        && value <= std::numeric_limits<std::int32_t>::max();
}

}

BloomRoiPlanResult planBloomRoi(
    const RectI sourceSupport,
    const RectI monitorBounds,
    const PyramidFootprint& footprint) noexcept
{
    BloomRoiPlanResult result{};
    result.plan.sourceSupport = sourceSupport;

    if (!isValidRect(sourceSupport) || !isValidRect(monitorBounds) || isEmpty(monitorBounds))
    {
        result.status = RoiStatus::InvalidRect;
        return result;
    }

    if (isEmpty(sourceSupport))
    {
        result.status = RoiStatus::Empty;
        return result;
    }

    // A signed 32-bit pixel domain cannot safely represent a period beyond 2^30.
    if (footprint.coarsestMipLevel > 30U)
    {
        result.status = RoiStatus::InvalidFootprint;
        return result;
    }

    std::uint64_t guardX = 0;
    std::uint64_t guardY = 0;
    for (const ReceptiveFieldTerm term : footprint.worstPath)
    {
        if (term.mipLevel > 30U || term.mipLevel > footprint.coarsestMipLevel)
        {
            result.status = RoiStatus::InvalidFootprint;
            return result;
        }

        guardX += static_cast<std::uint64_t>(term.radiusX) << term.mipLevel;
        guardY += static_cast<std::uint64_t>(term.radiusY) << term.mipLevel;
    }

    if (guardX > std::numeric_limits<std::uint32_t>::max()
        || guardY > std::numeric_limits<std::uint32_t>::max())
    {
        result.status = RoiStatus::IntegerOverflow;
        return result;
    }

    result.plan.guardX = static_cast<std::uint32_t>(guardX);
    result.plan.guardY = static_cast<std::uint32_t>(guardY);
    result.plan.phasePeriod = 1U << footprint.coarsestMipLevel;

    const std::int64_t inflatedLeft = static_cast<std::int64_t>(sourceSupport.left)
        - static_cast<std::int64_t>(guardX);
    const std::int64_t inflatedTop = static_cast<std::int64_t>(sourceSupport.top)
        - static_cast<std::int64_t>(guardY);
    const std::int64_t inflatedRight = static_cast<std::int64_t>(sourceSupport.right)
        + static_cast<std::int64_t>(guardX);
    const std::int64_t inflatedBottom = static_cast<std::int64_t>(sourceSupport.bottom)
        + static_cast<std::int64_t>(guardY);

    if (!fitsInt32(inflatedLeft) || !fitsInt32(inflatedTop)
        || !fitsInt32(inflatedRight) || !fitsInt32(inflatedBottom))
    {
        result.status = RoiStatus::IntegerOverflow;
        return result;
    }

    const RectI inflated{
        static_cast<std::int32_t>(inflatedLeft),
        static_cast<std::int32_t>(inflatedTop),
        static_cast<std::int32_t>(inflatedRight),
        static_cast<std::int32_t>(inflatedBottom)};
    result.plan.bloomOutput = intersect(inflated, monitorBounds);

    if (!isValidRect(result.plan.bloomOutput) || isEmpty(result.plan.bloomOutput))
    {
        result.status = RoiStatus::Empty;
        return result;
    }

    const std::int64_t period = result.plan.phasePeriod;
    const std::int64_t alignedLeft = alignDown(
        result.plan.bloomOutput.left,
        monitorBounds.left,
        period);
    const std::int64_t alignedTop = alignDown(
        result.plan.bloomOutput.top,
        monitorBounds.top,
        period);
    const std::int64_t alignedRight = alignUp(
        result.plan.bloomOutput.right,
        monitorBounds.left,
        period);
    const std::int64_t alignedBottom = alignUp(
        result.plan.bloomOutput.bottom,
        monitorBounds.top,
        period);

    const RectI aligned{
        static_cast<std::int32_t>(alignedLeft),
        static_cast<std::int32_t>(alignedTop),
        static_cast<std::int32_t>(alignedRight),
        static_cast<std::int32_t>(alignedBottom)};
    result.plan.alignedWork = intersect(aligned, monitorBounds);
    return result;
}

BloomRoiPlanResult planUnityBloomRoi(
    const RectI sourceSupport,
    const RectI monitorBounds,
    const UnityBloomPlan& bloomPlan) noexcept
{
    // FourTap samples are bilinear lookups at one texel (prefilter/downsample)
    // or SampleScale/2 texels (upsample/resolve). Two texels per side is a
    // conservative integer footprint for every currently supported preset,
    // including the bilinear half-texel support.
    constexpr std::uint16_t fixedFourTapRadius = 2U;
    if (bloomPlan.mipCount == 0U
        || bloomPlan.mipCount > unityBloomMaxMipCount
        || !std::isfinite(bloomPlan.sampleScale)
        || bloomPlan.sampleScale <= 0.0F)
    {
        return BloomRoiPlanResult{BloomRoiPlan{}, RoiStatus::InvalidFootprint};
    }

    const float upsampleRadius = std::ceil(
        bloomPlan.sampleScale * 0.5F + 0.5F);
    if (!std::isfinite(upsampleRadius)
        || upsampleRadius < 1.0F
        || upsampleRadius
            > static_cast<float>(std::numeric_limits<std::uint16_t>::max()))
    {
        return BloomRoiPlanResult{BloomRoiPlan{}, RoiStatus::InvalidFootprint};
    }

    const auto upsampleRadius16 = static_cast<std::uint16_t>(upsampleRadius);
    std::array<ReceptiveFieldTerm, unityBloomMaxMipCount * 2U + 2U> terms{};
    std::size_t termCount = 0U;
    const auto append = [&terms, &termCount](
                            const std::uint8_t mipLevel,
                            const std::uint16_t radius)
    {
        terms[termCount++] = ReceptiveFieldTerm{mipLevel, radius, radius};
    };

    // The first target is the source prefilter at Bloom mip level one.
    append(0U, fixedFourTapRadius);
    for (std::uint8_t index = 1U;
         index < bloomPlan.mipCount;
         ++index)
    {
        append(index, fixedFourTapRadius);
    }
    for (std::uint8_t coarseIndex = bloomPlan.mipCount - 1U;
         coarseIndex > 0U;
         --coarseIndex)
    {
        append(
            static_cast<std::uint8_t>(coarseIndex + 1U),
            upsampleRadius16);
    }
    // ResolveBloomResult samples the first Bloom target once more at full
    // resolution, so retain its fine-level dependency in the guard.
    append(1U, upsampleRadius16);

    return planBloomRoi(
        sourceSupport,
        monitorBounds,
        PyramidFootprint{
            std::span<const ReceptiveFieldTerm>(terms.data(), termCount),
            bloomPlan.mipCount});
}

RectI unite(const RectI lhs, const RectI rhs) noexcept
{
    return RectI{
        std::min(lhs.left, rhs.left),
        std::min(lhs.top, rhs.top),
        std::max(lhs.right, rhs.right),
        std::max(lhs.bottom, rhs.bottom)};
}

}
