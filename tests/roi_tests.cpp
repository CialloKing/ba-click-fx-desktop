#include "test_support.hpp"

#include "bafx/core/roi.hpp"

#include <array>
#include <cstdint>

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
