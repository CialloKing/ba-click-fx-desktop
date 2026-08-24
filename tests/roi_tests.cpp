#include "test_support.hpp"

#include "bafx/core/roi.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <random>

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
