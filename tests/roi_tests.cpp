#include "test_support.hpp"

#include "bafx/core/roi.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <vector>

using namespace bafx::core;

namespace
{

void checkRect(const RectI actual, const RectI expected)
{
    BAFX_CHECK(actual.left == expected.left);
    BAFX_CHECK(actual.top == expected.top);
    BAFX_CHECK(actual.right == expected.right);
    BAFX_CHECK(actual.bottom == expected.bottom);
}

[[nodiscard]] std::uint32_t referenceUnityBloomGuard(
    const UnityBloomPlan& plan)
{
    constexpr std::uint32_t fourTapRadius = 2U;
    const auto upsampleRadius = static_cast<std::uint32_t>(
        std::ceil(plan.sampleScale * 0.5F + 0.5F));
    std::uint64_t guard = fourTapRadius;
    for (std::uint8_t index = 1U; index < plan.mipCount; ++index)
    {
        guard += static_cast<std::uint64_t>(fourTapRadius) << index;
    }
    for (std::uint8_t coarse = plan.mipCount - 1U;
         coarse > 0U;
         --coarse)
    {
        guard += static_cast<std::uint64_t>(upsampleRadius)
            << (coarse + 1U);
    }
    guard += static_cast<std::uint64_t>(upsampleRadius) << 1U;
    return static_cast<std::uint32_t>(guard);
}

[[nodiscard]] RectI referenceAlignedRect(
    const RectI source,
    const RectI monitor,
    const std::uint32_t guard,
    const std::uint32_t period)
{
    const RectI bloom{
        std::max(
            monitor.left,
            static_cast<std::int32_t>(
                static_cast<std::int64_t>(source.left) - guard)),
        std::max(
            monitor.top,
            static_cast<std::int32_t>(
                static_cast<std::int64_t>(source.top) - guard)),
        std::min(
            monitor.right,
            static_cast<std::int32_t>(
                static_cast<std::int64_t>(source.right) + guard)),
        std::min(
            monitor.bottom,
            static_cast<std::int32_t>(
                static_cast<std::int64_t>(source.bottom) + guard))};
    const auto alignDownFrom = [period](
                                   const std::int32_t value,
                                   const std::int32_t origin)
    {
        const std::int64_t offset =
            static_cast<std::int64_t>(value) - origin;
        return static_cast<std::int32_t>(
            static_cast<std::int64_t>(origin)
            + offset / period * period);
    };
    const auto alignUpFrom = [period](
                                 const std::int32_t value,
                                 const std::int32_t origin)
    {
        const std::int64_t offset =
            static_cast<std::int64_t>(value) - origin;
        return static_cast<std::int32_t>(
            static_cast<std::int64_t>(origin)
            + (offset + period - 1U) / period * period);
    };
    return RectI{
        std::max(monitor.left, alignDownFrom(bloom.left, monitor.left)),
        std::max(monitor.top, alignDownFrom(bloom.top, monitor.top)),
        std::min(monitor.right, alignUpFrom(bloom.right, monitor.left)),
        std::min(monitor.bottom, alignUpFrom(bloom.bottom, monitor.top))};
}

struct AxisRange
{
    std::int32_t begin{0};
    std::int32_t end{0};
};

[[nodiscard]] AxisRange oracleSampleDependencyAxis(
    const std::int32_t destinationBegin,
    const std::int32_t destinationEnd,
    const std::int32_t destinationExtent,
    const std::int32_t sourceExtent,
    const long double sampleOffset)
{
    std::int32_t minimum = sourceExtent;
    std::int32_t maximum = -1;
    const std::array destinationPixels{
        destinationBegin,
        destinationEnd - 1};
    const std::array offsets{-sampleOffset, sampleOffset};
    for (const std::int32_t destinationPixel : destinationPixels)
    {
        for (const long double offset : offsets)
        {
            // This independently follows D3D's normalized texture coordinate
            // and bilinear footprint instead of the planner's integer guard.
            const long double coordinate =
                (static_cast<long double>(destinationPixel) + 0.5L)
                    * sourceExtent / destinationExtent
                - 0.5L + offset;
            const auto first = static_cast<std::int64_t>(
                std::floor(coordinate));
            for (const std::int64_t sample : {first, first + 1})
            {
                const auto clamped = static_cast<std::int32_t>(
                    std::clamp<std::int64_t>(sample, 0, sourceExtent - 1));
                minimum = std::min(minimum, clamped);
                maximum = std::max(maximum, clamped);
            }
        }
    }
    return AxisRange{minimum, maximum + 1};
}

[[nodiscard]] RectI oracleSampleDependency(
    const RectI destinationRect,
    const BloomExtent destinationExtent,
    const BloomExtent sourceExtent,
    const long double sampleOffset)
{
    const AxisRange horizontal = oracleSampleDependencyAxis(
        destinationRect.left,
        destinationRect.right,
        destinationExtent.width,
        sourceExtent.width,
        sampleOffset);
    const AxisRange vertical = oracleSampleDependencyAxis(
        destinationRect.top,
        destinationRect.bottom,
        destinationExtent.height,
        sourceExtent.height,
        sampleOffset);
    return RectI{
        horizontal.begin,
        vertical.begin,
        horizontal.end,
        vertical.end};
}

[[nodiscard]] bool containsRect(const RectI outer, const RectI inner)
{
    return outer.left <= inner.left
        && outer.top <= inner.top
        && outer.right >= inner.right
        && outer.bottom >= inner.bottom;
}

[[nodiscard]] bool containsPixel(
    const RectI rect,
    const std::int32_t x,
    const std::int32_t y)
{
    return x >= rect.left && x < rect.right
        && y >= rect.top && y < rect.bottom;
}

struct BooleanSupport
{
    BloomExtent extent{};
    std::vector<std::uint8_t> pixels{};
};

[[nodiscard]] std::size_t supportIndex(
    const BooleanSupport& support,
    const std::int32_t x,
    const std::int32_t y)
{
    return static_cast<std::size_t>(y)
            * static_cast<std::size_t>(support.extent.width)
        + static_cast<std::size_t>(x);
}

[[nodiscard]] bool supportAt(
    const BooleanSupport& support,
    const std::int32_t x,
    const std::int32_t y)
{
    return support.pixels[supportIndex(support, x, y)] != 0U;
}

void setSupport(
    BooleanSupport& support,
    const std::int32_t x,
    const std::int32_t y)
{
    support.pixels[supportIndex(support, x, y)] = 1U;
}

[[nodiscard]] BooleanSupport makeBooleanSupport(
    const BloomExtent extent,
    const RectI rect)
{
    BooleanSupport support{
        extent,
        std::vector<std::uint8_t>(
            static_cast<std::size_t>(extent.width)
                * static_cast<std::size_t>(extent.height),
            0U)};
    for (std::int32_t y = rect.top; y < rect.bottom; ++y)
    {
        for (std::int32_t x = rect.left; x < rect.right; ++x)
        {
            setSupport(support, x, y);
        }
    }
    return support;
}

[[nodiscard]] std::array<std::int32_t, 2> oracleBilinearAxis(
    const std::int32_t outputPixel,
    const std::int32_t outputExtent,
    const std::int32_t inputExtent,
    const long double offset)
{
    const long double coordinate =
        (static_cast<long double>(outputPixel) + 0.5L)
            * inputExtent / outputExtent
        - 0.5L + offset;
    const auto first = static_cast<std::int64_t>(std::floor(coordinate));
    const std::int64_t second = coordinate == std::floor(coordinate)
        ? first
        : first + 1;
    return {
        static_cast<std::int32_t>(
            std::clamp<std::int64_t>(first, 0, inputExtent - 1)),
        static_cast<std::int32_t>(
            std::clamp<std::int64_t>(second, 0, inputExtent - 1))};
}

template <typename Visitor>
void visitOracleSamples(
    const std::int32_t outputX,
    const std::int32_t outputY,
    const BloomExtent outputExtent,
    const BloomExtent inputExtent,
    const long double offset,
    const bool fourTap,
    Visitor&& visitor)
{
    const std::array offsets{-offset, offset};
    const std::size_t offsetCount = fourTap ? offsets.size() : 1U;
    for (std::size_t yOffsetIndex = 0U;
         yOffsetIndex < offsetCount;
         ++yOffsetIndex)
    {
        const long double yOffset = fourTap ? offsets[yOffsetIndex] : 0.0L;
        const auto sourceYs = oracleBilinearAxis(
            outputY,
            outputExtent.height,
            inputExtent.height,
            yOffset);
        for (std::size_t xOffsetIndex = 0U;
             xOffsetIndex < offsetCount;
             ++xOffsetIndex)
        {
            const long double xOffset = fourTap
                ? offsets[xOffsetIndex]
                : 0.0L;
            const auto sourceXs = oracleBilinearAxis(
                outputX,
                outputExtent.width,
                inputExtent.width,
                xOffset);
            for (const std::int32_t sourceY : sourceYs)
            {
                for (const std::int32_t sourceX : sourceXs)
                {
                    visitor(sourceX, sourceY);
                }
            }
        }
    }
}

[[nodiscard]] BooleanSupport propagateBooleanSupport(
    const BooleanSupport& input,
    const BloomExtent outputExtent,
    const long double offset,
    const bool fourTap)
{
    BooleanSupport output{
        outputExtent,
        std::vector<std::uint8_t>(
            static_cast<std::size_t>(outputExtent.width)
                * static_cast<std::size_t>(outputExtent.height),
            0U)};
    for (std::int32_t y = 0; y < outputExtent.height; ++y)
    {
        for (std::int32_t x = 0; x < outputExtent.width; ++x)
        {
            bool anyInput = false;
            visitOracleSamples(
                x,
                y,
                outputExtent,
                input.extent,
                offset,
                fourTap,
                [&input, &anyInput](
                    const std::int32_t sourceX,
                    const std::int32_t sourceY)
                {
                    anyInput = anyInput
                        || supportAt(input, sourceX, sourceY);
                });
            if (anyInput)
            {
                setSupport(output, x, y);
            }
        }
    }
    return output;
}

[[nodiscard]] BooleanSupport uniteBooleanSupport(
    BooleanSupport lhs,
    const BooleanSupport& rhs)
{
    BAFX_CHECK(lhs.extent.width == rhs.extent.width);
    BAFX_CHECK(lhs.extent.height == rhs.extent.height);
    for (std::size_t index = 0U; index < lhs.pixels.size(); ++index)
    {
        lhs.pixels[index] = static_cast<std::uint8_t>(
            lhs.pixels[index] != 0U || rhs.pixels[index] != 0U);
    }
    return lhs;
}

void checkBooleanSupportContained(
    const BooleanSupport& support,
    const RectI rect)
{
    for (std::int32_t y = 0; y < support.extent.height; ++y)
    {
        for (std::int32_t x = 0; x < support.extent.width; ++x)
        {
            if (supportAt(support, x, y))
            {
                BAFX_CHECK(containsPixel(rect, x, y));
            }
        }
    }
}

void checkBooleanDependencies(
    const BooleanSupport& output,
    const RectI outputRect,
    const BooleanSupport& input,
    const RectI inputRect,
    const long double offset,
    const bool fourTap)
{
    for (std::int32_t y = 0; y < output.extent.height; ++y)
    {
        for (std::int32_t x = 0; x < output.extent.width; ++x)
        {
            if (!supportAt(output, x, y))
            {
                continue;
            }
            BAFX_CHECK(containsPixel(outputRect, x, y));
            const RectI dependencyBounds = oracleSampleDependency(
                RectI{x, y, x + 1, y + 1},
                output.extent,
                input.extent,
                fourTap ? offset : 0.0L);
            visitOracleSamples(
                x,
                y,
                output.extent,
                input.extent,
                offset,
                fourTap,
                [&input, inputRect, dependencyBounds](
                    const std::int32_t sourceX,
                    const std::int32_t sourceY)
                {
                    BAFX_CHECK(containsPixel(
                        dependencyBounds,
                        sourceX,
                        sourceY));
                    if (supportAt(input, sourceX, sourceY))
                    {
                        BAFX_CHECK(containsPixel(
                            inputRect,
                            sourceX,
                            sourceY));
                    }
                });
        }
    }
}

struct BooleanBloomChain
{
    BooleanSupport source{};
    std::vector<BooleanSupport> down{};
    std::vector<BooleanSupport> up{};
    BooleanSupport resolve{};
};

[[nodiscard]] BooleanBloomChain simulateBooleanBloom(
    const BloomExtent monitorExtent,
    const RectI localSourceSupport,
    const UnityBloomPlan& bloom)
{
    BooleanBloomChain chain{};
    chain.source = makeBooleanSupport(monitorExtent, localSourceSupport);
    chain.down.reserve(bloom.mipCount);
    chain.down.push_back(propagateBooleanSupport(
        chain.source,
        bloom.mipChain[0],
        1.0L,
        true));
    for (std::size_t index = 1U; index < bloom.mipCount; ++index)
    {
        chain.down.push_back(propagateBooleanSupport(
            chain.down[index - 1U],
            bloom.mipChain[index],
            1.0L,
            true));
    }

    chain.up.resize(static_cast<std::size_t>(bloom.mipCount) - 1U);
    BooleanSupport accumulated = chain.down.back();
    const long double upsampleOffset =
        static_cast<long double>(bloom.sampleScale) * 0.5L;
    for (std::size_t coarseIndex = bloom.mipCount - 1U;
         coarseIndex > 0U;
         --coarseIndex)
    {
        const std::size_t fineIndex = coarseIndex - 1U;
        BooleanSupport coarse = propagateBooleanSupport(
            accumulated,
            bloom.mipChain[fineIndex],
            upsampleOffset,
            true);
        const BooleanSupport fine = propagateBooleanSupport(
            chain.down[fineIndex],
            bloom.mipChain[fineIndex],
            0.0L,
            false);
        chain.up[fineIndex] = uniteBooleanSupport(coarse, fine);
        accumulated = chain.up[fineIndex];
    }
    chain.resolve = propagateBooleanSupport(
        accumulated,
        monitorExtent,
        upsampleOffset,
        true);
    return chain;
}

void checkBooleanBloomPlan(
    const BooleanBloomChain& chain,
    const UnityBloomPassRoiPlan& roi,
    const UnityBloomPlan& bloom,
    const RectI localSourceSupport)
{
    const long double upsampleOffset =
        static_cast<long double>(bloom.sampleScale) * 0.5L;
    checkBooleanSupportContained(chain.down[0], roi.downRects[0]);
    checkBooleanDependencies(
        chain.down[0],
        roi.downRects[0],
        chain.source,
        localSourceSupport,
        1.0L,
        true);
    for (std::size_t index = 1U; index < bloom.mipCount; ++index)
    {
        checkBooleanSupportContained(
            chain.down[index],
            roi.downRects[index]);
        checkBooleanDependencies(
            chain.down[index],
            roi.downRects[index],
            chain.down[index - 1U],
            roi.downRects[index - 1U],
            1.0L,
            true);
    }

    for (std::size_t fineIndex = 0U;
         fineIndex + 1U < bloom.mipCount;
         ++fineIndex)
    {
        checkBooleanSupportContained(
            chain.up[fineIndex],
            roi.upRects[fineIndex]);
        const BooleanSupport& coarse = fineIndex + 2U < bloom.mipCount
            ? chain.up[fineIndex + 1U]
            : chain.down[fineIndex + 1U];
        const RectI coarseRect = fineIndex + 2U < bloom.mipCount
            ? roi.upRects[fineIndex + 1U]
            : roi.downRects[fineIndex + 1U];
        checkBooleanDependencies(
            chain.up[fineIndex],
            roi.upRects[fineIndex],
            coarse,
            coarseRect,
            upsampleOffset,
            true);
        checkBooleanDependencies(
            chain.up[fineIndex],
            roi.upRects[fineIndex],
            chain.down[fineIndex],
            roi.downRects[fineIndex],
            0.0L,
            false);
    }

    const BooleanSupport& accumulated = bloom.mipCount == 1U
        ? chain.down[0]
        : chain.up[0];
    const RectI accumulatedRect = bloom.mipCount == 1U
        ? roi.downRects[0]
        : roi.upRects[0];
    checkBooleanSupportContained(chain.resolve, roi.resolveRect);
    checkBooleanDependencies(
        chain.resolve,
        roi.resolveRect,
        accumulated,
        accumulatedRect,
        upsampleOffset,
        true);
}

[[nodiscard]] std::uint64_t rectArea(const RectI rect)
{
    return static_cast<std::uint64_t>(rect.right - rect.left)
        * static_cast<std::uint64_t>(rect.bottom - rect.top);
}

[[nodiscard]] std::uint64_t extentArea(const BloomExtent extent)
{
    return static_cast<std::uint64_t>(extent.width)
        * static_cast<std::uint64_t>(extent.height);
}

void checkLocalRect(const RectI rect, const BloomExtent extent)
{
    BAFX_CHECK(rect.left >= 0);
    BAFX_CHECK(rect.top >= 0);
    BAFX_CHECK(rect.right > rect.left);
    BAFX_CHECK(rect.bottom > rect.top);
    BAFX_CHECK(rect.right <= extent.width);
    BAFX_CHECK(rect.bottom <= extent.height);
}

void checkUnityBloomPassDependencies(
    const UnityBloomPassRoiPlan& roi,
    const UnityBloomPlan& bloom,
    const BloomExtent monitorExtent)
{
    BAFX_CHECK(roi.mipCount == bloom.mipCount);
    checkLocalRect(roi.resolveRect, monitorExtent);

    for (std::size_t fineIndex = 0U;
         fineIndex + 1U < bloom.mipCount;
         ++fineIndex)
    {
        checkLocalRect(roi.upRects[fineIndex], bloom.mipChain[fineIndex]);
    }

    for (std::size_t index = 0U; index < bloom.mipCount; ++index)
    {
        checkLocalRect(roi.downRects[index], bloom.mipChain[index]);
    }

    std::uint64_t expectedPyramidFull = 0U;
    std::uint64_t expectedPyramidCandidate = 0U;
    for (std::size_t index = 1U; index < bloom.mipCount; ++index)
    {
        expectedPyramidFull += extentArea(bloom.mipChain[index]);
        expectedPyramidCandidate += rectArea(roi.downRects[index]);
    }
    for (std::size_t index = 0U; index + 1U < bloom.mipCount; ++index)
    {
        expectedPyramidFull += extentArea(bloom.mipChain[index]);
        expectedPyramidCandidate += rectArea(roi.upRects[index]);
    }
    BAFX_CHECK(roi.prefilterPixels.fullPixels == extentArea(bloom.mipChain[0]));
    BAFX_CHECK(roi.prefilterPixels.candidatePixels == rectArea(roi.downRects[0]));
    BAFX_CHECK(roi.pyramidPixels.fullPixels == expectedPyramidFull);
    BAFX_CHECK(roi.pyramidPixels.candidatePixels == expectedPyramidCandidate);
    BAFX_CHECK(roi.resolvePixels.fullPixels == extentArea(monitorExtent));
    BAFX_CHECK(roi.resolvePixels.candidatePixels == rectArea(roi.resolveRect));
    BAFX_CHECK(
        roi.totalPixels.fullPixels
        == roi.prefilterPixels.fullPixels
            + roi.pyramidPixels.fullPixels
            + roi.resolvePixels.fullPixels);
    BAFX_CHECK(
        roi.totalPixels.candidatePixels
        == roi.prefilterPixels.candidatePixels
            + roi.pyramidPixels.candidatePixels
            + roi.resolvePixels.candidatePixels);
    BAFX_CHECK(roi.totalPixels.candidatePixels <= roi.totalPixels.fullPixels);
}

}

BAFX_TEST(roi_accumulates_full_receptive_path_and_preserves_phase)
{
    constexpr std::array terms{
        ReceptiveFieldTerm{0, 1, 1},
        ReceptiveFieldTerm{1, 1, 2},
        ReceptiveFieldTerm{2, 2, 1}};
    const auto result = planBloomRoi(
        RectI{101, 55, 122, 68},
        RectI{0, 0, 1920, 1080},
        PyramidFootprint{terms, 2});

    BAFX_CHECK(result.status == RoiStatus::Ok);
    BAFX_CHECK(result.plan.guardX == 11U);
    BAFX_CHECK(result.plan.guardY == 9U);
    BAFX_CHECK(result.plan.phasePeriod == 4U);
    checkRect(result.plan.bloomOutput, RectI{90, 46, 133, 77});
    checkRect(result.plan.alignedWork, RectI{88, 44, 136, 80});
}

BAFX_TEST(roi_clips_at_monitor_edge_without_changing_fullscreen_phase)
{
    constexpr std::array terms{
        ReceptiveFieldTerm{0, 1, 1},
        ReceptiveFieldTerm{1, 1, 2},
        ReceptiveFieldTerm{2, 2, 1}};
    const auto result = planBloomRoi(
        RectI{0, 0, 3, 3},
        RectI{0, 0, 1920, 1080},
        PyramidFootprint{terms, 2});

    BAFX_CHECK(result.status == RoiStatus::Ok);
    checkRect(result.plan.bloomOutput, RectI{0, 0, 14, 12});
    checkRect(result.plan.alignedWork, RectI{0, 0, 16, 12});
}

BAFX_TEST(roi_phase_is_relative_to_monitor_origin)
{
    constexpr std::array terms{ReceptiveFieldTerm{2, 0, 0}};
    const auto result = planBloomRoi(
        RectI{-97, 3, -91, 7},
        RectI{-100, 0, 100, 100},
        PyramidFootprint{terms, 2});

    BAFX_CHECK(result.status == RoiStatus::Ok);
    checkRect(result.plan.alignedWork, RectI{-100, 0, -88, 8});
}

BAFX_TEST(roi_rejects_invalid_footprints_and_unites_dirty_rects)
{
    constexpr std::array terms{ReceptiveFieldTerm{3, 1, 1}};
    const auto result = planBloomRoi(
        RectI{0, 0, 10, 10},
        RectI{0, 0, 100, 100},
        PyramidFootprint{terms, 2});
    BAFX_CHECK(result.status == RoiStatus::InvalidFootprint);

    checkRect(
        unite(RectI{10, 20, 30, 40}, RectI{5, 25, 35, 45}),
        RectI{5, 20, 35, 45});
}

BAFX_TEST(roi_unity_bloom_plan_uses_shader_footprint_and_phase)
{
    const auto bloom = planUnityBloom(
        BloomExtent{1950, 1097},
        UnityBloomSettings{7.0F, 0.0F, 1.7F});
    BAFX_CHECK(bloom.status == UnityBloomStatus::Ok);

    const auto result = planUnityBloomRoi(
        RectI{960, 530, 990, 560},
        RectI{0, 0, 1950, 1097},
        bloom.plan);
    BAFX_CHECK(result.status == RoiStatus::Ok);
    BAFX_CHECK(result.plan.guardX == 378U);
    BAFX_CHECK(result.plan.guardY == 378U);
    BAFX_CHECK(result.plan.phasePeriod == 64U);
    checkRect(result.plan.bloomOutput, RectI{582, 152, 1368, 938});
    checkRect(result.plan.alignedWork, RectI{576, 128, 1408, 960});
}

BAFX_TEST(roi_unity_bloom_plan_rejects_unusable_shader_plan)
{
    UnityBloomPlan invalid{};
    BAFX_CHECK(
        planUnityBloomRoi(
            RectI{0, 0, 10, 10},
            RectI{0, 0, 100, 100},
            invalid)
            .status
        == RoiStatus::InvalidFootprint);

    invalid.mipCount = 1U;
    invalid.sampleScale = 0.0F;
    BAFX_CHECK(
        planUnityBloomRoi(
            RectI{0, 0, 10, 10},
            RectI{0, 0, 100, 100},
            invalid)
            .status
        == RoiStatus::InvalidFootprint);
}

BAFX_TEST(roi_adaptive_path_uses_50_percent_entry_and_65_percent_exit)
{
    BAFX_CHECK(
        decideActiveFxRoiAdaptivePath(false, 500U, 1000U)
        == ActiveFxRoiAdaptiveDecision::Apply);
    BAFX_CHECK(
        decideActiveFxRoiAdaptivePath(false, 501U, 1000U)
        == ActiveFxRoiAdaptiveDecision::BenefitTooSmall);
    BAFX_CHECK(
        decideActiveFxRoiAdaptivePath(true, 650U, 1000U)
        == ActiveFxRoiAdaptiveDecision::Apply);
    BAFX_CHECK(
        decideActiveFxRoiAdaptivePath(true, 651U, 1000U)
        == ActiveFxRoiAdaptiveDecision::BenefitTooSmall);
    BAFX_CHECK(
        decideActiveFxRoiAdaptivePath(true, 799U, 1000U)
        == ActiveFxRoiAdaptiveDecision::BenefitTooSmall);
    BAFX_CHECK(
        decideActiveFxRoiAdaptivePath(true, 800U, 1000U)
        == ActiveFxRoiAdaptiveDecision::AreaTooLarge);
}

BAFX_TEST(roi_adaptive_path_rejects_invalid_counts_without_overflow)
{
    BAFX_CHECK(
        decideActiveFxRoiAdaptivePath(false, 0U, 1000U)
        == ActiveFxRoiAdaptiveDecision::BenefitTooSmall);
    BAFX_CHECK(
        decideActiveFxRoiAdaptivePath(false, 1U, 0U)
        == ActiveFxRoiAdaptiveDecision::BenefitTooSmall);
    BAFX_CHECK(
        decideActiveFxRoiAdaptivePath(false, 1001U, 1000U)
        == ActiveFxRoiAdaptiveDecision::BenefitTooSmall);

    constexpr std::uint64_t maximum =
        std::numeric_limits<std::uint64_t>::max();
    BAFX_CHECK(
        decideActiveFxRoiAdaptivePath(false, maximum / 2U, maximum)
        == ActiveFxRoiAdaptiveDecision::Apply);
    BAFX_CHECK(
        decideActiveFxRoiAdaptivePath(true, maximum / 2U, maximum)
        == ActiveFxRoiAdaptiveDecision::Apply);
}

BAFX_TEST(roi_unity_bloom_matches_fixed_seed_reference_for_10000_rectangles)
{
    constexpr std::array diffusions{4.0F, 6.0F, 7.0F, 10.0F};
    std::mt19937_64 random(0xBAF00206ULL);
    for (std::size_t iteration = 0U; iteration < 10'000U; ++iteration)
    {
        const std::int32_t width = 65 + static_cast<std::int32_t>(
            random() % 4032U);
        const std::int32_t height = 65 + static_cast<std::int32_t>(
            random() % 4032U);
        const std::int32_t originX = -4096 + static_cast<std::int32_t>(
            random() % 8193U);
        const std::int32_t originY = -4096 + static_cast<std::int32_t>(
            random() % 8193U);
        const RectI monitor{
            originX,
            originY,
            originX + width,
            originY + height};
        const std::int32_t left = originX + static_cast<std::int32_t>(
            random() % static_cast<std::uint64_t>(width));
        const std::int32_t top = originY + static_cast<std::int32_t>(
            random() % static_cast<std::uint64_t>(height));
        const std::int32_t right = left + 1
            + static_cast<std::int32_t>(
                random()
                % static_cast<std::uint64_t>(monitor.right - left));
        const std::int32_t bottom = top + 1
            + static_cast<std::int32_t>(
                random()
                % static_cast<std::uint64_t>(monitor.bottom - top));
        const RectI source{left, top, right, bottom};
        const UnityBloomPlanResult bloom = planUnityBloom(
            BloomExtent{width, height},
            UnityBloomSettings{
                diffusions[iteration % diffusions.size()],
                0.0F,
                1.0F});
        BAFX_CHECK(bloom.status == UnityBloomStatus::Ok);

        const BloomRoiPlanResult actual = planUnityBloomRoi(
            source,
            monitor,
            bloom.plan);
        BAFX_CHECK(actual.status == RoiStatus::Ok);
        const std::uint32_t guard = referenceUnityBloomGuard(bloom.plan);
        BAFX_CHECK(actual.plan.guardX == guard);
        BAFX_CHECK(actual.plan.guardY == guard);
        BAFX_CHECK(actual.plan.phasePeriod == (1U << bloom.plan.mipCount));
        checkRect(
            actual.plan.alignedWork,
            referenceAlignedRect(
                source,
                monitor,
                guard,
                actual.plan.phasePeriod));
    }
}

BAFX_TEST(roi_rejects_signed_coordinate_inflation_overflow)
{
    constexpr std::array terms{ReceptiveFieldTerm{0U, 1U, 1U}};
    constexpr RectI coordinateDomain{
        std::numeric_limits<std::int32_t>::min(),
        std::numeric_limits<std::int32_t>::min(),
        std::numeric_limits<std::int32_t>::max(),
        std::numeric_limits<std::int32_t>::max()};
    BAFX_CHECK(
        planBloomRoi(
            RectI{
                std::numeric_limits<std::int32_t>::min(),
                0,
                std::numeric_limits<std::int32_t>::min() + 1,
                1},
            coordinateDomain,
            PyramidFootprint{terms, 0U})
            .status
        == RoiStatus::IntegerOverflow);
    BAFX_CHECK(
        planBloomRoi(
            RectI{
                std::numeric_limits<std::int32_t>::max() - 1,
                0,
                std::numeric_limits<std::int32_t>::max(),
                1},
            coordinateDomain,
            PyramidFootprint{terms, 0U})
            .status
        == RoiStatus::IntegerOverflow);
}

BAFX_TEST(roi_unity_bloom_pass_plan_uses_target_local_support_and_pixel_totals)
{
    constexpr BloomExtent monitorExtent{1950, 1097};
    const UnityBloomPlanResult bloom = planUnityBloom(
        monitorExtent,
        UnityBloomSettings{7.0F, 0.0F, 1.7F});
    BAFX_CHECK(bloom.status == UnityBloomStatus::Ok);
    const UnityBloomPassRoiPlanResult result = planUnityBloomPassRoi(
        RectI{860, 580, 890, 610},
        RectI{-100, 50, 1850, 1147},
        bloom.plan);

    BAFX_CHECK(result.status == RoiStatus::Ok);
    checkRect(result.plan.resolveRect, RectI{770, 322, 1180, 775});
    checkRect(result.plan.basePlan.alignedWork, RectI{476, 178, 1308, 1010});
    checkUnityBloomPassDependencies(result.plan, bloom.plan, monitorExtent);
}

BAFX_TEST(roi_unity_bloom_pass_plan_contains_moving_dirty_rects)
{
    constexpr BloomExtent monitorExtent{3840, 2160};
    constexpr RectI monitor{0, 0, monitorExtent.width, monitorExtent.height};
    constexpr RectI previous{1000, 800, 1040, 840};
    constexpr RectI current{1050, 820, 1090, 860};
    const UnityBloomPlanResult bloom = planUnityBloom(
        monitorExtent,
        UnityBloomSettings{7.0F, 0.0F, 1.7F});
    BAFX_CHECK(bloom.status == UnityBloomStatus::Ok);
    const UnityBloomPassRoiPlanResult previousPlan =
        planUnityBloomPassRoi(previous, monitor, bloom.plan);
    const UnityBloomPassRoiPlanResult currentPlan =
        planUnityBloomPassRoi(current, monitor, bloom.plan);
    const UnityBloomPassRoiPlanResult movingPlan = planUnityBloomPassRoi(
        unite(previous, current),
        monitor,
        bloom.plan);
    BAFX_CHECK(previousPlan.status == RoiStatus::Ok);
    BAFX_CHECK(currentPlan.status == RoiStatus::Ok);
    BAFX_CHECK(movingPlan.status == RoiStatus::Ok);
    BAFX_CHECK(containsRect(
        movingPlan.plan.resolveRect,
        previousPlan.plan.resolveRect));
    BAFX_CHECK(containsRect(
        movingPlan.plan.resolveRect,
        currentPlan.plan.resolveRect));
    for (std::size_t index = 0U; index < bloom.plan.mipCount; ++index)
    {
        BAFX_CHECK(containsRect(
            movingPlan.plan.downRects[index],
            previousPlan.plan.downRects[index]));
        BAFX_CHECK(containsRect(
            movingPlan.plan.downRects[index],
            currentPlan.plan.downRects[index]));
        if (index + 1U < bloom.plan.mipCount)
        {
            BAFX_CHECK(containsRect(
                movingPlan.plan.upRects[index],
                previousPlan.plan.upRects[index]));
            BAFX_CHECK(containsRect(
                movingPlan.plan.upRects[index],
                currentPlan.plan.upRects[index]));
        }
    }
}

BAFX_TEST(roi_unity_bloom_pass_plan_avoids_double_guard_for_4k_center_support)
{
    constexpr BloomExtent monitorExtent{3840, 2160};
    const UnityBloomPlanResult bloom = planUnityBloom(
        monitorExtent,
        UnityBloomSettings{7.0F, 0.0F, 1.7F});
    BAFX_CHECK(bloom.status == UnityBloomStatus::Ok);
    const UnityBloomPassRoiPlanResult result = planUnityBloomPassRoi(
        RectI{1880, 1040, 1960, 1120},
        RectI{0, 0, monitorExtent.width, monitorExtent.height},
        bloom.plan);
    BAFX_CHECK(result.status == RoiStatus::Ok);

    // A forward support pass keeps the 80x80 source near its true mip-0
    // footprint instead of applying the complete Bloom guard a second time.
    BAFX_CHECK(
        result.plan.prefilterPixels.candidatePixels * 100U
        < result.plan.prefilterPixels.fullPixels);
    BAFX_CHECK(
        result.plan.pyramidPixels.candidatePixels * 100U
        <= result.plan.pyramidPixels.fullPixels * 45U);
    BAFX_CHECK(
        result.plan.totalPixels.candidatePixels * 100U
        <= result.plan.totalPixels.fullPixels * 45U);
}

BAFX_TEST(roi_unity_bloom_pass_plan_covers_edges_odd_sizes_and_diffusions)
{
    constexpr std::array extents{
        BloomExtent{255, 257},
        BloomExtent{256, 258},
        BloomExtent{511, 320},
        BloomExtent{512, 321}};
    constexpr std::array diffusions{4.0F, 6.0F, 7.0F, 10.0F};
    for (const BloomExtent extent : extents)
    {
        const RectI monitor{-37, 91, -37 + extent.width, 91 + extent.height};
        const std::array supports{
            RectI{monitor.left, monitor.top, monitor.left + 1, monitor.top + 1},
            RectI{monitor.right - 1, monitor.top, monitor.right, monitor.top + 1},
            RectI{monitor.left, monitor.bottom - 1, monitor.left + 1, monitor.bottom},
            RectI{monitor.right - 1, monitor.bottom - 1, monitor.right, monitor.bottom}};
        for (const float diffusion : diffusions)
        {
            const UnityBloomPlanResult bloom = planUnityBloom(
                extent,
                UnityBloomSettings{diffusion, 0.0F, 1.7F});
            BAFX_CHECK(bloom.status == UnityBloomStatus::Ok);
            for (const RectI support : supports)
            {
                const UnityBloomPassRoiPlanResult result =
                    planUnityBloomPassRoi(support, monitor, bloom.plan);
                BAFX_CHECK(result.status == RoiStatus::Ok);
                checkUnityBloomPassDependencies(result.plan, bloom.plan, extent);
            }
        }
    }
}

BAFX_TEST(roi_unity_bloom_pass_plan_matches_sampling_oracle_for_random_rects)
{
    constexpr std::array diffusions{4.0F, 6.0F, 7.0F, 10.0F};
    std::mt19937_64 random(0xBAF00207ULL);
    for (std::size_t iteration = 0U; iteration < 2'000U; ++iteration)
    {
        const std::int32_t width = 65 + static_cast<std::int32_t>(
            random() % 448U);
        const std::int32_t height = 65 + static_cast<std::int32_t>(
            random() % 448U);
        const std::int32_t originX = -1024 + static_cast<std::int32_t>(
            random() % 2049U);
        const std::int32_t originY = -1024 + static_cast<std::int32_t>(
            random() % 2049U);
        const RectI monitor{
            originX,
            originY,
            originX + width,
            originY + height};
        const std::int32_t left = originX + static_cast<std::int32_t>(
            random() % static_cast<std::uint64_t>(width));
        const std::int32_t top = originY + static_cast<std::int32_t>(
            random() % static_cast<std::uint64_t>(height));
        const RectI support{
            left,
            top,
            left + 1 + static_cast<std::int32_t>(
                random() % static_cast<std::uint64_t>(monitor.right - left)),
            top + 1 + static_cast<std::int32_t>(
                random() % static_cast<std::uint64_t>(monitor.bottom - top))};
        const UnityBloomPlanResult bloom = planUnityBloom(
            BloomExtent{width, height},
            UnityBloomSettings{
                diffusions[iteration % diffusions.size()],
                0.0F,
                1.7F});
        BAFX_CHECK(bloom.status == UnityBloomStatus::Ok);
        const UnityBloomPassRoiPlanResult result = planUnityBloomPassRoi(
            support,
            monitor,
            bloom.plan);
        BAFX_CHECK(result.status == RoiStatus::Ok);
        checkUnityBloomPassDependencies(
            result.plan,
            bloom.plan,
            BloomExtent{width, height});
    }
}

BAFX_TEST(roi_unity_bloom_pass_plan_contains_boolean_forward_support)
{
    constexpr std::array diffusions{4.0F, 6.0F, 7.0F, 10.0F};
    std::mt19937_64 random(0xBAF0F207ULL);
    for (std::size_t iteration = 0U; iteration < 1'000U; ++iteration)
    {
        const std::int32_t width = 9 + static_cast<std::int32_t>(
            random() % 48U);
        const std::int32_t height = 9 + static_cast<std::int32_t>(
            random() % 48U);
        RectI localSupport{};
        switch (iteration % 8U)
        {
        case 0U:
            localSupport = RectI{0, 0, 1, 1};
            break;
        case 1U:
            localSupport = RectI{width - 1, 0, width, 1};
            break;
        case 2U:
            localSupport = RectI{0, height - 1, 1, height};
            break;
        case 3U:
            localSupport = RectI{
                width - 1,
                height - 1,
                width,
                height};
            break;
        default:
        {
            const std::int32_t left = static_cast<std::int32_t>(
                random() % static_cast<std::uint64_t>(width));
            const std::int32_t top = static_cast<std::int32_t>(
                random() % static_cast<std::uint64_t>(height));
            localSupport = RectI{
                left,
                top,
                left + 1 + static_cast<std::int32_t>(
                    random() % static_cast<std::uint64_t>(width - left)),
                top + 1 + static_cast<std::int32_t>(
                    random() % static_cast<std::uint64_t>(height - top))};
            break;
        }
        }
        const std::int32_t originX = -100 + static_cast<std::int32_t>(
            random() % 201U);
        const std::int32_t originY = -100 + static_cast<std::int32_t>(
            random() % 201U);
        const RectI monitor{
            originX,
            originY,
            originX + width,
            originY + height};
        const RectI globalSupport{
            originX + localSupport.left,
            originY + localSupport.top,
            originX + localSupport.right,
            originY + localSupport.bottom};
        const BloomExtent monitorExtent{width, height};
        const UnityBloomPlanResult bloom = planUnityBloom(
            monitorExtent,
            UnityBloomSettings{
                diffusions[iteration % diffusions.size()],
                0.0F,
                1.7F});
        BAFX_CHECK(bloom.status == UnityBloomStatus::Ok);
        const UnityBloomPassRoiPlanResult result = planUnityBloomPassRoi(
            globalSupport,
            monitor,
            bloom.plan);
        BAFX_CHECK(result.status == RoiStatus::Ok);

        const BooleanBloomChain oracle = simulateBooleanBloom(
            monitorExtent,
            localSupport,
            bloom.plan);
        checkBooleanBloomPlan(
            oracle,
            result.plan,
            bloom.plan,
            localSupport);
    }
}

BAFX_TEST(roi_unity_bloom_pass_plan_uses_forward_resolve_for_odd_mip_extents)
{
    constexpr BloomExtent monitorExtent{15, 44};
    constexpr RectI sourceSupport{11, 35, 13, 41};
    const UnityBloomPlanResult bloom = planUnityBloom(
        monitorExtent,
        UnityBloomSettings{4.0F, 0.0F, 1.7F});
    BAFX_CHECK(bloom.status == UnityBloomStatus::Ok);
    BAFX_CHECK(bloom.plan.mipCount == 1U);
    const UnityBloomPassRoiPlanResult result = planUnityBloomPassRoi(
        sourceSupport,
        RectI{0, 0, monitorExtent.width, monitorExtent.height},
        bloom.plan);
    BAFX_CHECK(result.status == RoiStatus::Ok);

    // Repeated floor division makes mip 0 wider than an exact half-scale.
    // The legacy power-of-two guard misses x=6, which receives nonzero weight.
    BAFX_CHECK(result.plan.basePlan.bloomOutput.left == 7);
    BAFX_CHECK(result.plan.resolveRect.left == 6);
    const BooleanBloomChain oracle = simulateBooleanBloom(
        monitorExtent,
        sourceSupport,
        bloom.plan);
    BAFX_CHECK(supportAt(oracle.resolve, 6, 32));
    checkBooleanBloomPlan(
        oracle,
        result.plan,
        bloom.plan,
        sourceSupport);
}

BAFX_TEST(roi_unity_bloom_pass_plan_fails_closed_for_empty_invalid_and_overflow)
{
    const UnityBloomPlanResult bloom = planUnityBloom(
        BloomExtent{1920, 1080},
        UnityBloomSettings{7.0F, 0.0F, 1.7F});
    BAFX_CHECK(bloom.status == UnityBloomStatus::Ok);
    BAFX_CHECK(
        planUnityBloomPassRoi(
            RectI{10, 10, 10, 20},
            RectI{0, 0, 1920, 1080},
            bloom.plan)
            .status
        == RoiStatus::Empty);

    UnityBloomPlan invalid = bloom.plan;
    invalid.mipChain[1].width += 1;
    BAFX_CHECK(
        planUnityBloomPassRoi(
            RectI{10, 10, 20, 20},
            RectI{0, 0, 1920, 1080},
            invalid)
            .status
        == RoiStatus::InvalidFootprint);
    invalid = bloom.plan;
    invalid.mipChain[0].height = 0;
    BAFX_CHECK(
        planUnityBloomPassRoi(
            RectI{10, 10, 20, 20},
            RectI{0, 0, 1920, 1080},
            invalid)
            .status
        == RoiStatus::InvalidFootprint);

    constexpr RectI oversizedMonitor{
        std::numeric_limits<std::int32_t>::min(),
        std::numeric_limits<std::int32_t>::min(),
        std::numeric_limits<std::int32_t>::max(),
        std::numeric_limits<std::int32_t>::max()};
    BAFX_CHECK(
        planUnityBloomPassRoi(
            RectI{0, 0, 1, 1},
            oversizedMonitor,
            bloom.plan)
            .status
        == RoiStatus::IntegerOverflow);
}
