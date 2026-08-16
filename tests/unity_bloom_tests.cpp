#include "test_support.hpp"

#include "bafx/core/unity_bloom.hpp"

#include <limits>

using namespace bafx::core;

BAFX_TEST(unity_bloom_matches_the_verified_golden_plan)
{
    const auto result = planUnityBloom(
        BloomExtent{1950, 1097},
        UnityBloomSettings{7.0F, 0.0F, 1.7F});

    BAFX_CHECK(result.status == UnityBloomStatus::Ok);
    BAFX_CHECK(result.plan.mipCount == 6U);
    BAFX_CHECK(result.plan.mipChain[0].width == 975);
    BAFX_CHECK(result.plan.mipChain[0].height == 548);
    BAFX_CHECK(result.plan.mipChain[1].width == 487);
    BAFX_CHECK(result.plan.mipChain[1].height == 274);
    BAFX_CHECK(result.plan.mipChain[2].width == 243);
    BAFX_CHECK(result.plan.mipChain[2].height == 137);
    BAFX_CHECK(result.plan.mipChain[3].width == 121);
    BAFX_CHECK(result.plan.mipChain[3].height == 68);
    BAFX_CHECK(result.plan.mipChain[4].width == 60);
    BAFX_CHECK(result.plan.mipChain[4].height == 34);
    BAFX_CHECK(result.plan.mipChain[5].width == 30);
    BAFX_CHECK(result.plan.mipChain[5].height == 17);
    BAFX_CHECK_NEAR(result.plan.sampleScale, 1.42925835F, 1.0e-7F);
    BAFX_CHECK_NEAR(result.plan.exposureGain, 0.1250584847F, 1.0e-7F);
}

BAFX_TEST(unity_bloom_anamorphic_ratio_stretches_one_initial_axis)
{
    auto result = planUnityBloom(
        BloomExtent{9, 7},
        UnityBloomSettings{7.0F, -1.0F, 1.7F});
    BAFX_CHECK(result.status == UnityBloomStatus::Ok);
    BAFX_CHECK(result.plan.mipChain[0].width == 9);
    BAFX_CHECK(result.plan.mipChain[0].height == 3);

    result = planUnityBloom(
        BloomExtent{9, 7},
        UnityBloomSettings{7.0F, 1.0F, 1.7F});
    BAFX_CHECK(result.plan.mipChain[0].width == 4);
    BAFX_CHECK(result.plan.mipChain[0].height == 7);

    // Runtime values outside the inspector range saturate like Mathf.Clamp.
    result = planUnityBloom(
        BloomExtent{9, 7},
        UnityBloomSettings{7.0F, 4.0F, 1.7F});
    BAFX_CHECK(result.plan.mipChain[0].width == 4);
    BAFX_CHECK(result.plan.mipChain[0].height == 7);
}

BAFX_TEST(unity_bloom_preserves_clamped_iteration_and_fraction_rules)
{
    auto result = planUnityBloom(
        BloomExtent{1, 1},
        UnityBloomSettings{0.0F, 0.0F, 0.0F});
    BAFX_CHECK(result.status == UnityBloomStatus::Ok);
    BAFX_CHECK(result.plan.mipCount == 1U);
    BAFX_CHECK(result.plan.mipChain[0].width == 1);
    BAFX_CHECK(result.plan.mipChain[0].height == 1);
    BAFX_CHECK_NEAR(result.plan.sampleScale, 0.5F, 0.0F);
    BAFX_CHECK_NEAR(result.plan.exposureGain, 0.0F, 0.0F);

    result = planUnityBloom(
        BloomExtent{
            std::numeric_limits<std::int32_t>::max(),
            std::numeric_limits<std::int32_t>::max()},
        UnityBloomSettings{10.0F, 0.0F, 1.7F});
    BAFX_CHECK(result.status == UnityBloomStatus::Ok);
    BAFX_CHECK(result.plan.mipCount == unityBloomMaxMipCount);
    BAFX_CHECK(result.plan.mipChain[15].width == 32767);
    BAFX_CHECK(result.plan.mipChain[15].height == 32767);
    BAFX_CHECK_NEAR(result.plan.sampleScale, 0.5F, 0.0F);

    const auto saturatedDiffusion = planUnityBloom(
        BloomExtent{1950, 1097},
        UnityBloomSettings{100.0F, 0.0F, 1.7F});
    const auto maximumDiffusion = planUnityBloom(
        BloomExtent{1950, 1097},
        UnityBloomSettings{10.0F, 0.0F, 1.7F});
    BAFX_CHECK(saturatedDiffusion.status == UnityBloomStatus::Ok);
    BAFX_CHECK(saturatedDiffusion.plan.mipCount == maximumDiffusion.plan.mipCount);
    BAFX_CHECK_NEAR(
        saturatedDiffusion.plan.sampleScale,
        maximumDiffusion.plan.sampleScale,
        0.0F);
}

BAFX_TEST(unity_bloom_rejects_invalid_inputs_without_a_partial_plan)
{
    auto result = planUnityBloom(BloomExtent{0, 1080});
    BAFX_CHECK(result.status == UnityBloomStatus::InvalidSourceExtent);
    BAFX_CHECK(result.plan.mipCount == 0U);

    result = planUnityBloom(BloomExtent{1920, -1});
    BAFX_CHECK(result.status == UnityBloomStatus::InvalidSourceExtent);

    result = planUnityBloom(
        BloomExtent{1920, 1080},
        UnityBloomSettings{-0.5F, 0.0F, 1.7F});
    BAFX_CHECK(result.status == UnityBloomStatus::InvalidDiffusion);

    result = planUnityBloom(
        BloomExtent{1920, 1080},
        UnityBloomSettings{
            std::numeric_limits<float>::infinity(),
            0.0F,
            1.7F});
    BAFX_CHECK(result.status == UnityBloomStatus::InvalidDiffusion);

    result = planUnityBloom(
        BloomExtent{1920, 1080},
        UnityBloomSettings{
            std::numeric_limits<float>::quiet_NaN(),
            0.0F,
            1.7F});
    BAFX_CHECK(result.status == UnityBloomStatus::InvalidDiffusion);

    result = planUnityBloom(
        BloomExtent{1920, 1080},
        UnityBloomSettings{
            7.0F,
            std::numeric_limits<float>::infinity(),
            1.7F});
    BAFX_CHECK(result.status == UnityBloomStatus::InvalidAnamorphicRatio);

    result = planUnityBloom(
        BloomExtent{1920, 1080},
        UnityBloomSettings{
            7.0F,
            std::numeric_limits<float>::quiet_NaN(),
            1.7F});
    BAFX_CHECK(result.status == UnityBloomStatus::InvalidAnamorphicRatio);

    result = planUnityBloom(
        BloomExtent{1920, 1080},
        UnityBloomSettings{7.0F, 0.0F, -1.0F});
    BAFX_CHECK(result.status == UnityBloomStatus::InvalidIntensity);

    result = planUnityBloom(
        BloomExtent{1920, 1080},
        UnityBloomSettings{
            7.0F,
            0.0F,
            std::numeric_limits<float>::quiet_NaN()});
    BAFX_CHECK(result.status == UnityBloomStatus::InvalidIntensity);

    result = planUnityBloom(
        BloomExtent{1920, 1080},
        UnityBloomSettings{
            7.0F,
            0.0F,
            std::numeric_limits<float>::infinity()});
    BAFX_CHECK(result.status == UnityBloomStatus::InvalidIntensity);

    result = planUnityBloom(
        BloomExtent{1920, 1080},
        UnityBloomSettings{
            7.0F,
            0.0F,
            std::numeric_limits<float>::max()});
    BAFX_CHECK(result.status == UnityBloomStatus::InvalidIntensity);
    BAFX_CHECK(result.plan.mipCount == 0U);
}
