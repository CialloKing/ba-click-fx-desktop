#include "test_support.hpp"

#include "bafx/core/material.hpp"

#include <limits>

using namespace bafx::core;

BAFX_TEST(material_sanitize_keeps_coverage_separate_from_energy)
{
    const MaterialOutputs input{
        Float3{0.4F, 0.7F, -0.1F},
        0.5F,
        Float3{-0.1F, 2.0F, 20.0F},
        Float3{1.0F, std::numeric_limits<float>::quiet_NaN(), 2.0F}};
    const auto result = sanitizeMaterialOutputs(
        input,
        MaterialLimits{1.0F, 8.0F, 8.0F});

    BAFX_CHECK(result.status == MaterialSanitizeStatus::Sanitized);
    BAFX_CHECK_NEAR(result.outputs.normalPremultiplied.r, 0.4F, 0.0F);
    BAFX_CHECK_NEAR(result.outputs.normalPremultiplied.g, 0.5F, 0.0F);
    BAFX_CHECK_NEAR(result.outputs.normalPremultiplied.b, 0.0F, 0.0F);
    BAFX_CHECK_NEAR(result.outputs.directEmission.r, 0.0F, 0.0F);
    BAFX_CHECK_NEAR(result.outputs.directEmission.g, 2.0F, 0.0F);
    BAFX_CHECK_NEAR(result.outputs.directEmission.b, 8.0F, 0.0F);
    BAFX_CHECK_NEAR(result.outputs.bloomSeed.r, 0.0F, 0.0F);
    BAFX_CHECK_NEAR(result.outputs.bloomSeed.g, 0.0F, 0.0F);
    BAFX_CHECK_NEAR(result.outputs.bloomSeed.b, 0.0F, 0.0F);
}

BAFX_TEST(final_composition_makes_layer_order_explicit)
{
    const MaterialOutputs material{
        Float3{0.1F, 0.2F, 0.3F},
        0.25F,
        Float3{0.4F, 0.2F, 0.1F},
        Float3{}};
    OutputPolicy policy{};
    policy.extendedPremultipliedVerified = true;

    auto pixel = makeOverlayPixel(material, Float3{0.2F, 0.1F, 0.1F}, policy);
    BAFX_CHECK_NEAR(pixel.premultipliedRgb.r, 0.7F, 1.0e-6F);
    BAFX_CHECK_NEAR(pixel.premultipliedRgb.g, 0.5F, 1.0e-6F);
    BAFX_CHECK_NEAR(pixel.premultipliedRgb.b, 0.5F, 1.0e-6F);

    policy.emissionLayerOrder = EmissionLayerOrder::BehindCoverage;
    pixel = makeOverlayPixel(material, Float3{0.2F, 0.1F, 0.1F}, policy);
    BAFX_CHECK_NEAR(pixel.premultipliedRgb.r, 0.55F, 1.0e-6F);
    BAFX_CHECK_NEAR(pixel.premultipliedRgb.g, 0.425F, 1.0e-6F);
    BAFX_CHECK_NEAR(pixel.premultipliedRgb.b, 0.45F, 1.0e-6F);
}

BAFX_TEST(unverified_or_disabled_extended_output_drops_only_additive_energy)
{
    const MaterialOutputs material{
        Float3{0.1F, 0.2F, 0.3F},
        0.25F,
        Float3{4.0F, 4.0F, 4.0F},
        Float3{}};
    OutputPolicy policy{};

    auto pixel = makeOverlayPixel(material, Float3{2.0F, 2.0F, 2.0F}, policy);
    BAFX_CHECK_NEAR(pixel.premultipliedRgb.r, 0.1F, 0.0F);
    BAFX_CHECK_NEAR(pixel.alpha, 0.25F, 0.0F);

    policy.extendedPremultipliedVerified = true;
    policy.sdrEmissionBehavior = SdrEmissionBehavior::DisableExtendedEmission;
    pixel = makeOverlayPixel(material, Float3{2.0F, 2.0F, 2.0F}, policy);
    BAFX_CHECK_NEAR(pixel.premultipliedRgb.g, 0.2F, 0.0F);
}

