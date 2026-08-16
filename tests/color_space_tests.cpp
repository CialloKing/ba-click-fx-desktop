#include "test_support.hpp"

#include "bafx/core/color_space.hpp"

#include <cmath>

using namespace bafx::core;

BAFX_TEST(srgb_to_linear_matches_the_unity_active_color_space_transfer)
{
    BAFX_CHECK_NEAR(srgbToLinearChannel(-1.0F), 0.0F, 0.0F);
    BAFX_CHECK_NEAR(srgbToLinearChannel(0.0F), 0.0F, 0.0F);
    BAFX_CHECK_NEAR(srgbToLinearChannel(0.04045F), 0.003130805F, 1.0e-8F);
    BAFX_CHECK_NEAR(srgbToLinearChannel(0.5F), 0.21404114F, 1.0e-7F);
    BAFX_CHECK_NEAR(srgbToLinearChannel(1.0F), 1.0F, 0.0F);
    BAFX_CHECK_NEAR(srgbToLinearChannel(2.0F), 1.0F, 0.0F);

    const Float3 converted = srgbToLinear(Float3{0.25F, 0.5F, 0.75F});
    BAFX_CHECK_NEAR(converted.r, 0.05087609F, 1.0e-7F);
    BAFX_CHECK_NEAR(converted.g, 0.21404114F, 1.0e-7F);
    BAFX_CHECK_NEAR(converted.b, 0.52252156F, 1.0e-7F);
}

BAFX_TEST(unity_gamma_bloom_parameters_preserve_extended_hdr_conversion)
{
    BAFX_CHECK_NEAR(unityGammaToLinearChannel(0.0F), 0.0F, 0.0F);
    BAFX_CHECK_NEAR(
        unityGammaToLinearChannel(0.04045F),
        0.003130805F,
        1.0e-8F);
    BAFX_CHECK_NEAR(
        unityGammaToLinearChannel(0.5F),
        0.21404114F,
        1.0e-7F);
    BAFX_CHECK_NEAR(unityGammaToLinearChannel(1.0F), 1.0F, 0.0F);
    BAFX_CHECK_NEAR(
        unityGammaToLinearChannel(2.0F),
        std::pow(2.0F, 2.2F),
        1.0e-6F);
    BAFX_CHECK_NEAR(
        unityGammaToLinearChannel(65504.0F),
        halfFloatMaximum,
        0.0F);
}
