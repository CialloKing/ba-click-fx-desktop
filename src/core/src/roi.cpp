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

[[nodiscard]] std::uint64_t ratioFloor(
    const std::uint64_t value,
    const std::uint64_t numerator,
    const std::uint64_t denominator) noexcept
{
    // Split before multiplying so a deliberately adversarial pixel count
    // cannot overflow the adaptive gate and accidentally enable ROI.
    const std::uint64_t quotient = value / denominator;
    const std::uint64_t remainder = value % denominator;
    return quotient * numerator + remainder * numerator / denominator;
}

[[nodiscard]] bool checkedAdd(
    const std::int64_t lhs,
    const std::int64_t rhs,
    std::int64_t& result) noexcept
{
    if ((rhs > 0 && lhs > std::numeric_limits<std::int64_t>::max() - rhs)
        || (rhs < 0 && lhs < std::numeric_limits<std::int64_t>::min() - rhs))
    {
        return false;
    }
    result = lhs + rhs;
    return true;
}

[[nodiscard]] bool checkedMultiplyNonNegative(
    const std::int64_t lhs,
    const std::int64_t rhs,
    std::int64_t& result) noexcept
{
    if (lhs < 0 || rhs < 0
        || (lhs != 0 && rhs > std::numeric_limits<std::int64_t>::max() / lhs))
    {
        return false;
    }
    result = lhs * rhs;
    return true;
}

[[nodiscard]] RoiStatus dependencyInterval(
    const std::int32_t destinationBegin,
    const std::int32_t destinationEnd,
    const std::int32_t destinationExtent,
    const std::int32_t sourceExtent,
    const std::uint32_t radius,
    std::int32_t& sourceBegin,
    std::int32_t& sourceEnd) noexcept
{
    if (destinationExtent <= 0 || sourceExtent <= 0
        || destinationBegin < 0 || destinationEnd > destinationExtent
        || destinationBegin >= destinationEnd)
    {
        return RoiStatus::InvalidRect;
    }

    std::int64_t scaledBegin = 0;
    std::int64_t scaledEnd = 0;
    if (!checkedMultiplyNonNegative(
            destinationBegin,
            sourceExtent,
            scaledBegin)
        || !checkedMultiplyNonNegative(
            destinationEnd,
            sourceExtent,
            scaledEnd))
    {
        return RoiStatus::IntegerOverflow;
    }

    // Map the destination pixel edges through normalized UV space before
    // expanding. Applying the radius to pixel centers can omit the second
    // bilinear texel when odd extents place that center across a texel edge.
    std::int64_t roundedEnd = 0;
    if (!checkedAdd(scaledEnd, destinationExtent - 1, roundedEnd))
    {
        return RoiStatus::IntegerOverflow;
    }
    std::int64_t unclippedBegin = scaledBegin / destinationExtent;
    std::int64_t unclippedEnd = roundedEnd / destinationExtent;
    if (!checkedAdd(
            unclippedBegin,
            -static_cast<std::int64_t>(radius),
            unclippedBegin)
        || !checkedAdd(
            unclippedEnd,
            static_cast<std::int64_t>(radius),
            unclippedEnd))
    {
        return RoiStatus::IntegerOverflow;
    }

    const std::int64_t clippedBegin = std::clamp<std::int64_t>(
        unclippedBegin,
        0,
        sourceExtent);
    const std::int64_t clippedEnd = std::clamp<std::int64_t>(
        unclippedEnd,
        0,
        sourceExtent);
    if (clippedBegin >= clippedEnd)
    {
        return RoiStatus::Empty;
    }
    sourceBegin = static_cast<std::int32_t>(clippedBegin);
    sourceEnd = static_cast<std::int32_t>(clippedEnd);
    return RoiStatus::Ok;
}

[[nodiscard]] RoiStatus dependencyRect(
    const RectI destinationRect,
    const BloomExtent destinationExtent,
    const BloomExtent sourceExtent,
    const std::uint32_t radius,
    RectI& result) noexcept
{
    const RoiStatus horizontal = dependencyInterval(
        destinationRect.left,
        destinationRect.right,
        destinationExtent.width,
        sourceExtent.width,
        radius,
        result.left,
        result.right);
    if (horizontal != RoiStatus::Ok)
    {
        return horizontal;
    }
    return dependencyInterval(
        destinationRect.top,
        destinationRect.bottom,
        destinationExtent.height,
        sourceExtent.height,
        radius,
        result.top,
        result.bottom);
}

[[nodiscard]] RoiStatus propagateSupportInterval(
    const std::int32_t inputBegin,
    const std::int32_t inputEnd,
    const std::int32_t inputExtent,
    const std::int32_t outputExtent,
    const float sampleOffset,
    std::int32_t& outputBegin,
    std::int32_t& outputEnd) noexcept
{
    if (inputExtent <= 0 || outputExtent <= 0
        || inputBegin < 0 || inputEnd > inputExtent
        || inputBegin >= inputEnd
        || !std::isfinite(sampleOffset) || sampleOffset < 0.0F)
    {
        return RoiStatus::InvalidRect;
    }

    const auto centerAt = [inputExtent, outputExtent](
                              const std::int32_t outputPixel,
                              long double& center)
    {
        std::int64_t doubled = 0;
        std::int64_t product = 0;
        std::int64_t numerator = 0;
        std::int64_t denominator = 0;
        if (!checkedMultiplyNonNegative(outputPixel, 2, doubled)
            || !checkedAdd(doubled, 1, doubled)
            || !checkedMultiplyNonNegative(doubled, inputExtent, product)
            || !checkedAdd(product, -outputExtent, numerator)
            || !checkedMultiplyNonNegative(outputExtent, 2, denominator))
        {
            return RoiStatus::IntegerOverflow;
        }
        center = static_cast<long double>(numerator)
            / static_cast<long double>(denominator);
        return std::isfinite(center)
            ? RoiStatus::Ok
            : RoiStatus::IntegerOverflow;
    };
    const long double offset = sampleOffset;
    const auto reachesLowerSupport = [
                                         inputBegin,
                                         offset,
                                         &centerAt](
                                         const std::int32_t outputPixel,
                                         bool& reaches)
    {
        long double center = 0.0L;
        const RoiStatus status = centerAt(outputPixel, center);
        if (status != RoiStatus::Ok)
        {
            return status;
        }
        // Clamp addressing makes the first texel reachable from every sample
        // position beyond the left edge. Otherwise bilinear weight is nonzero
        // only while the rightmost tap is strictly past inputBegin - 1. Round
        // outward so CPU precision can never trim a GPU boundary sample.
        long double rightmost = center + offset;
        rightmost = std::nextafter(
            std::nextafter(
                rightmost,
                std::numeric_limits<long double>::infinity()),
            std::numeric_limits<long double>::infinity());
        reaches = inputBegin == 0
            || rightmost > static_cast<long double>(inputBegin - 1);
        return RoiStatus::Ok;
    };
    const auto remainsBeforeUpperSupport = [
                                               inputEnd,
                                               inputExtent,
                                               offset,
                                               &centerAt](
                                               const std::int32_t outputPixel,
                                               bool& remains)
    {
        long double center = 0.0L;
        const RoiStatus status = centerAt(outputPixel, center);
        if (status != RoiStatus::Ok)
        {
            return status;
        }
        // The last texel is likewise extended by clamp addressing. Away from
        // that border, the leftmost tap must stay strictly below inputEnd.
        long double leftmost = center - offset;
        leftmost = std::nextafter(
            std::nextafter(
                leftmost,
                -std::numeric_limits<long double>::infinity()),
            -std::numeric_limits<long double>::infinity());
        remains = inputEnd == inputExtent
            || leftmost < static_cast<long double>(inputEnd);
        return RoiStatus::Ok;
    };

    // Sample centers move monotonically with unchanged full-target UVs. Binary
    // search their nonzero kernel intersection instead of scanning a 4K target.
    std::int32_t lower = 0;
    std::int32_t upper = outputExtent;
    while (lower < upper)
    {
        const std::int32_t middle = lower + (upper - lower) / 2;
        bool reaches = false;
        const RoiStatus status = reachesLowerSupport(middle, reaches);
        if (status != RoiStatus::Ok)
        {
            return status;
        }
        if (!reaches)
        {
            lower = middle + 1;
        }
        else
        {
            upper = middle;
        }
    }
    outputBegin = lower;

    lower = outputBegin;
    upper = outputExtent;
    while (lower < upper)
    {
        const std::int32_t middle = lower + (upper - lower) / 2;
        bool remains = false;
        const RoiStatus status = remainsBeforeUpperSupport(middle, remains);
        if (status != RoiStatus::Ok)
        {
            return status;
        }
        if (remains)
        {
            lower = middle + 1;
        }
        else
        {
            upper = middle;
        }
    }
    outputEnd = lower;
    if (outputBegin >= outputEnd)
    {
        return RoiStatus::Empty;
    }

    bool reaches = false;
    bool remains = false;
    const RoiStatus lowerStatus = reachesLowerSupport(outputBegin, reaches);
    const RoiStatus upperStatus = remainsBeforeUpperSupport(
        outputBegin,
        remains);
    if (lowerStatus != RoiStatus::Ok)
    {
        return lowerStatus;
    }
    if (upperStatus != RoiStatus::Ok)
    {
        return upperStatus;
    }
    if (!reaches || !remains)
    {
        return RoiStatus::Empty;
    }
    return RoiStatus::Ok;
}

[[nodiscard]] RoiStatus propagateSupportRect(
    const RectI inputSupport,
    const BloomExtent inputExtent,
    const BloomExtent outputExtent,
    const float sampleOffset,
    RectI& result) noexcept
{
    const RoiStatus horizontal = propagateSupportInterval(
        inputSupport.left,
        inputSupport.right,
        inputExtent.width,
        outputExtent.width,
        sampleOffset,
        result.left,
        result.right);
    if (horizontal != RoiStatus::Ok)
    {
        return horizontal;
    }
    return propagateSupportInterval(
        inputSupport.top,
        inputSupport.bottom,
        inputExtent.height,
        outputExtent.height,
        sampleOffset,
        result.top,
        result.bottom);
}

[[nodiscard]] bool checkedAddPixels(
    const std::uint64_t value,
    std::uint64_t& total) noexcept
{
    if (value > std::numeric_limits<std::uint64_t>::max() - total)
    {
        return false;
    }
    total += value;
    return true;
}

[[nodiscard]] bool rectPixels(
    const RectI rect,
    std::uint64_t& pixels) noexcept
{
    if (!isValidRect(rect) || isEmpty(rect))
    {
        return false;
    }
    const std::uint64_t width = static_cast<std::uint64_t>(
        static_cast<std::int64_t>(rect.right) - rect.left);
    const std::uint64_t height = static_cast<std::uint64_t>(
        static_cast<std::int64_t>(rect.bottom) - rect.top);
    if (width != 0U
        && height > std::numeric_limits<std::uint64_t>::max() / width)
    {
        return false;
    }
    pixels = width * height;
    return true;
}

[[nodiscard]] bool extentPixels(
    const BloomExtent extent,
    std::uint64_t& pixels) noexcept
{
    if (extent.width <= 0 || extent.height <= 0)
    {
        return false;
    }
    return rectPixels(RectI{0, 0, extent.width, extent.height}, pixels);
}

[[nodiscard]] bool addPassPixels(
    const BloomExtent fullExtent,
    const RectI candidateRect,
    UnityBloomPassPixelTotals& totals) noexcept
{
    std::uint64_t fullPixels = 0;
    std::uint64_t candidatePixels = 0;
    return extentPixels(fullExtent, fullPixels)
        && rectPixels(candidateRect, candidatePixels)
        && checkedAddPixels(fullPixels, totals.fullPixels)
        && checkedAddPixels(candidatePixels, totals.candidatePixels);
}

[[nodiscard]] bool addPixelTotals(
    const UnityBloomPassPixelTotals source,
    UnityBloomPassPixelTotals& destination) noexcept
{
    return checkedAddPixels(source.fullPixels, destination.fullPixels)
        && checkedAddPixels(source.candidatePixels, destination.candidatePixels);
}

}

ActiveFxRoiAdaptiveDecision decideActiveFxRoiAdaptivePath(
    const bool previouslyActive,
    const std::uint64_t candidatePixels,
    const std::uint64_t fullTargetPixels) noexcept
{
    if (fullTargetPixels == 0U || candidatePixels == 0U
        || candidatePixels > fullTargetPixels)
    {
        return ActiveFxRoiAdaptiveDecision::BenefitTooSmall;
    }

    // Preserve the explicit large-area diagnostic independently from the
    // adaptive benefit gate. This makes a near-full target distinguishable
    // from the 50-65% hysteresis band in engineering telemetry.
    if (candidatePixels >= ratioFloor(fullTargetPixels, 4U, 5U))
    {
        return ActiveFxRoiAdaptiveDecision::AreaTooLarge;
    }

    const std::uint64_t maximumPixels = previouslyActive
        ? ratioFloor(fullTargetPixels, 13U, 20U)
        : ratioFloor(fullTargetPixels, 1U, 2U);
    return candidatePixels <= maximumPixels
        ? ActiveFxRoiAdaptiveDecision::Apply
        : ActiveFxRoiAdaptiveDecision::BenefitTooSmall;
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

UnityBloomPassRoiPlanResult planUnityBloomPassRoi(
    const RectI sourceSupport,
    const RectI monitorBounds,
    const UnityBloomPlan& bloomPlan) noexcept
{
    UnityBloomPassRoiPlanResult result{};
    const BloomRoiPlanResult base = planUnityBloomRoi(
        sourceSupport,
        monitorBounds,
        bloomPlan);
    result.plan.basePlan = base.plan;
    if (base.status != RoiStatus::Ok)
    {
        result.status = base.status;
        return result;
    }

    const std::int64_t monitorWidth64 =
        static_cast<std::int64_t>(monitorBounds.right) - monitorBounds.left;
    const std::int64_t monitorHeight64 =
        static_cast<std::int64_t>(monitorBounds.bottom) - monitorBounds.top;
    if (monitorWidth64 <= 0 || monitorHeight64 <= 0
        || monitorWidth64 > std::numeric_limits<std::int32_t>::max()
        || monitorHeight64 > std::numeric_limits<std::int32_t>::max())
    {
        result.status = RoiStatus::IntegerOverflow;
        return result;
    }
    const BloomExtent monitorExtent{
        static_cast<std::int32_t>(monitorWidth64),
        static_cast<std::int32_t>(monitorHeight64)};

    for (std::size_t index = 0U; index < bloomPlan.mipCount; ++index)
    {
        const BloomExtent extent = bloomPlan.mipChain[index];
        if (extent.width <= 0 || extent.height <= 0
            || extent.width > monitorExtent.width
            || extent.height > monitorExtent.height)
        {
            result.status = RoiStatus::InvalidFootprint;
            return result;
        }
        if (index > 0U)
        {
            const BloomExtent previous = bloomPlan.mipChain[index - 1U];
            if (extent.width != std::max(1, previous.width / 2)
                || extent.height != std::max(1, previous.height / 2))
            {
                result.status = RoiStatus::InvalidFootprint;
                return result;
            }
        }
    }

    const float radiusValue = std::ceil(
        bloomPlan.sampleScale * 0.5F + 0.5F);
    if (!std::isfinite(radiusValue) || radiusValue < 1.0F
        || radiusValue
            > static_cast<float>(std::numeric_limits<std::uint32_t>::max()))
    {
        result.status = RoiStatus::InvalidFootprint;
        return result;
    }
    const auto upsampleRadius = static_cast<std::uint32_t>(radiusValue);
    constexpr std::uint32_t downsampleRadius = 2U;
    constexpr std::uint32_t linearSampleRadius = 1U;
    constexpr float downsampleOffset = 1.0F;
    constexpr float linearSampleOffset = 0.0F;
    const float upsampleOffset = bloomPlan.sampleScale * 0.5F;

    const RectI clippedSourceSupport = intersect(sourceSupport, monitorBounds);
    if (!isValidRect(clippedSourceSupport) || isEmpty(clippedSourceSupport))
    {
        result.status = RoiStatus::Empty;
        return result;
    }
    const RectI localSourceSupport{
        static_cast<std::int32_t>(
            static_cast<std::int64_t>(clippedSourceSupport.left)
            - monitorBounds.left),
        static_cast<std::int32_t>(
            static_cast<std::int64_t>(clippedSourceSupport.top)
            - monitorBounds.top),
        static_cast<std::int32_t>(
            static_cast<std::int64_t>(clippedSourceSupport.right)
            - monitorBounds.left),
        static_cast<std::int32_t>(
            static_cast<std::int64_t>(clippedSourceSupport.bottom)
            - monitorBounds.top)};

    std::array<RectI, unityBloomMaxMipCount> forwardDownRects{};
    std::array<RectI, unityBloomMaxMipCount - 1U> forwardUpRects{};
    result.status = propagateSupportRect(
        localSourceSupport,
        monitorExtent,
        bloomPlan.mipChain[0],
        downsampleOffset,
        forwardDownRects[0]);
    if (result.status != RoiStatus::Ok)
    {
        return result;
    }
    for (std::size_t index = 1U; index < bloomPlan.mipCount; ++index)
    {
        result.status = propagateSupportRect(
            forwardDownRects[index - 1U],
            bloomPlan.mipChain[index - 1U],
            bloomPlan.mipChain[index],
            downsampleOffset,
            forwardDownRects[index]);
        if (result.status != RoiStatus::Ok)
        {
            return result;
        }
    }

    RectI forwardAccumulated =
        forwardDownRects[bloomPlan.mipCount - 1U];
    for (std::size_t coarseIndex = bloomPlan.mipCount - 1U;
         coarseIndex > 0U;
         --coarseIndex)
    {
        const std::size_t fineIndex = coarseIndex - 1U;
        RectI coarseSupport{};
        RectI fineSupport{};
        result.status = propagateSupportRect(
            forwardAccumulated,
            bloomPlan.mipChain[coarseIndex],
            bloomPlan.mipChain[fineIndex],
            upsampleOffset,
            coarseSupport);
        if (result.status != RoiStatus::Ok)
        {
            return result;
        }
        result.status = propagateSupportRect(
            forwardDownRects[fineIndex],
            bloomPlan.mipChain[fineIndex],
            bloomPlan.mipChain[fineIndex],
            linearSampleOffset,
            fineSupport);
        if (result.status != RoiStatus::Ok)
        {
            return result;
        }
        forwardUpRects[fineIndex] = unite(coarseSupport, fineSupport);
        forwardAccumulated = forwardUpRects[fineIndex];
    }

    RectI forwardResolveRect{};
    result.status = propagateSupportRect(
        forwardAccumulated,
        bloomPlan.mipChain[0],
        monitorExtent,
        upsampleOffset,
        forwardResolveRect);
    if (result.status != RoiStatus::Ok)
    {
        return result;
    }

    // The legacy power-of-two guard remains in basePlan for diagnostics, but
    // odd mip extents can make their normalized scale slightly larger than two.
    // Use the full-UV forward result as the semantic resolve support so that
    // those edge pixels are never clipped by the legacy approximation.
    result.plan.resolveRect = forwardResolveRect;
    if (!isValidRect(result.plan.resolveRect)
        || isEmpty(result.plan.resolveRect))
    {
        result.status = RoiStatus::Empty;
        return result;
    }
    result.plan.mipCount = bloomPlan.mipCount;

    RectI requiredAccumulated{};
    result.status = dependencyRect(
        result.plan.resolveRect,
        monitorExtent,
        bloomPlan.mipChain[0],
        upsampleRadius,
        requiredAccumulated);
    if (result.status != RoiStatus::Ok)
    {
        return result;
    }

    std::array<bool, unityBloomMaxMipCount> hasDownRect{};
    const auto includeDownRect = [&result, &hasDownRect](
                                     const std::size_t index,
                                     const RectI rect)
    {
        if (hasDownRect[index])
        {
            result.plan.downRects[index] = unite(
                result.plan.downRects[index],
                rect);
        }
        else
        {
            result.plan.downRects[index] = rect;
            hasDownRect[index] = true;
        }
    };

    if (bloomPlan.mipCount == 1U)
    {
        includeDownRect(0U, requiredAccumulated);
    }
    else
    {
        // Walk resolve toward the coarsest level. Each upsample reads both the
        // accumulated coarse image and its original downsampled fine image.
        for (std::size_t fineIndex = 0U;
             fineIndex + 1U < bloomPlan.mipCount;
             ++fineIndex)
        {
            result.plan.upRects[fineIndex] = requiredAccumulated;

            RectI fineDependency{};
            result.status = dependencyRect(
                requiredAccumulated,
                bloomPlan.mipChain[fineIndex],
                bloomPlan.mipChain[fineIndex],
                linearSampleRadius,
                fineDependency);
            if (result.status != RoiStatus::Ok)
            {
                return result;
            }
            includeDownRect(fineIndex, fineDependency);

            RectI coarseDependency{};
            result.status = dependencyRect(
                requiredAccumulated,
                bloomPlan.mipChain[fineIndex],
                bloomPlan.mipChain[fineIndex + 1U],
                upsampleRadius,
                coarseDependency);
            if (result.status != RoiStatus::Ok)
            {
                return result;
            }
            if (fineIndex + 2U < bloomPlan.mipCount)
            {
                requiredAccumulated = coarseDependency;
            }
            else
            {
                includeDownRect(fineIndex + 1U, coarseDependency);
            }
        }
    }

    // Every required coarse down target adds a dependency to the preceding
    // level. Union it with the fine branch already required by upsampling.
    for (std::size_t index = bloomPlan.mipCount - 1U;
         index > 0U;
         --index)
    {
        if (!hasDownRect[index])
        {
            result.status = RoiStatus::Empty;
            return result;
        }
        RectI previousDependency{};
        result.status = dependencyRect(
            result.plan.downRects[index],
            bloomPlan.mipChain[index],
            bloomPlan.mipChain[index - 1U],
            downsampleRadius,
            previousDependency);
        if (result.status != RoiStatus::Ok)
        {
            return result;
        }
        includeDownRect(index - 1U, previousDependency);
    }

    // Backward rectangles answer what the retained resolve pixels can read;
    // forward rectangles answer what can be nonzero. Their intersection omits
    // known-zero work while retaining every contributing dependency.
    for (std::size_t index = 0U; index < bloomPlan.mipCount; ++index)
    {
        if (hasDownRect[index])
        {
            result.plan.downRects[index] = intersect(
                result.plan.downRects[index],
                forwardDownRects[index]);
        }
        if (!hasDownRect[index]
            || !isValidRect(result.plan.downRects[index])
            || isEmpty(result.plan.downRects[index]))
        {
            result.status = RoiStatus::Empty;
            return result;
        }
    }
    for (std::size_t index = 0U; index + 1U < bloomPlan.mipCount; ++index)
    {
        result.plan.upRects[index] = intersect(
            result.plan.upRects[index],
            forwardUpRects[index]);
        if (!isValidRect(result.plan.upRects[index])
            || isEmpty(result.plan.upRects[index]))
        {
            result.status = RoiStatus::Empty;
            return result;
        }
    }

    if (!addPassPixels(
            bloomPlan.mipChain[0],
            result.plan.downRects[0],
            result.plan.prefilterPixels))
    {
        result.status = RoiStatus::IntegerOverflow;
        return result;
    }
    for (std::size_t index = 1U; index < bloomPlan.mipCount; ++index)
    {
        if (!addPassPixels(
                bloomPlan.mipChain[index],
                result.plan.downRects[index],
                result.plan.pyramidPixels))
        {
            result.status = RoiStatus::IntegerOverflow;
            return result;
        }
    }
    for (std::size_t index = 0U; index + 1U < bloomPlan.mipCount; ++index)
    {
        if (!addPassPixels(
                bloomPlan.mipChain[index],
                result.plan.upRects[index],
                result.plan.pyramidPixels))
        {
            result.status = RoiStatus::IntegerOverflow;
            return result;
        }
    }
    if (!addPassPixels(
            monitorExtent,
            result.plan.resolveRect,
            result.plan.resolvePixels)
        || !addPixelTotals(
            result.plan.prefilterPixels,
            result.plan.totalPixels)
        || !addPixelTotals(
            result.plan.pyramidPixels,
            result.plan.totalPixels)
        || !addPixelTotals(
            result.plan.resolvePixels,
            result.plan.totalPixels))
    {
        result.status = RoiStatus::IntegerOverflow;
        return result;
    }
    result.status = RoiStatus::Ok;
    return result;
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
