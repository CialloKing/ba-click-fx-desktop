#include "test_support.hpp"

#include "bafx/core/intensity.hpp"

#include <limits>

using namespace bafx::core;

BAFX_TEST(artistic_intensity_remains_dimensionless)
{
    OutputPolicy policy{};
    policy.artisticToCanonical = 0.5F;
    const auto result = resolveIntensity(
        AuthoredIntensity{5.992157F, IntensitySemantics::ArtisticRelative},
        policy);

    BAFX_CHECK(result.status == ResolveStatus::Ok);
    BAFX_CHECK(!result.approximate);
    BAFX_CHECK_NEAR(result.canonicalValue, 2.9960785F, 1.0e-5F);
}

BAFX_TEST(reference_white_uses_monitor_white_only_for_hdr)
{
    OutputPolicy policy{};
    policy.encoding = OutputEncoding::HdrSceneReferredScRgb;
    policy.hdrSdrWhiteNits = 200.0F;
    auto result = resolveIntensity(
        AuthoredIntensity{2.0F, IntensitySemantics::ReferenceWhiteRelative},
        policy);
    BAFX_CHECK_NEAR(result.canonicalValue, 5.0F, 1.0e-6F);

    policy.encoding = OutputEncoding::SdrDisplayRelative;
    result = resolveIntensity(
        AuthoredIntensity{2.0F, IntensitySemantics::ReferenceWhiteRelative},
        policy);
    BAFX_CHECK_NEAR(result.canonicalValue, 2.0F, 1.0e-6F);
}

BAFX_TEST(absolute_nits_require_an_explicit_output_path)
{
    OutputPolicy policy{};
    policy.encoding = OutputEncoding::HdrSceneReferredScRgb;
    auto result = resolveIntensity(
        AuthoredIntensity{1000.0F, IntensitySemantics::AbsoluteNits},
        policy);
    BAFX_CHECK_NEAR(result.canonicalValue, 12.5F, 1.0e-6F);

    policy.encoding = OutputEncoding::SdrDisplayRelative;
    result = resolveIntensity(
        AuthoredIntensity{100.0F, IntensitySemantics::AbsoluteNits},
        policy);
    BAFX_CHECK(result.status == ResolveStatus::UnsupportedOnOutput);
    BAFX_CHECK_NEAR(result.canonicalValue, 0.0F, 0.0F);

    policy.absoluteNitsOnSdr = AbsoluteNitsOnSdr::UseCalibratedPeak;
    policy.calibratedSdrPeakNits = 200.0F;
    result = resolveIntensity(
        AuthoredIntensity{100.0F, IntensitySemantics::AbsoluteNits},
        policy);
    BAFX_CHECK(result.approximate);
    BAFX_CHECK_NEAR(result.canonicalValue, 0.5F, 1.0e-6F);
}

BAFX_TEST(intensity_rejects_invalid_values_and_reports_clamp)
{
    OutputPolicy policy{};
    policy.maxCanonicalIntensity = 4.0F;
    auto result = resolveIntensity(
        AuthoredIntensity{10.0F, IntensitySemantics::ArtisticRelative},
        policy);
    BAFX_CHECK(result.clamped);
    BAFX_CHECK_NEAR(result.canonicalValue, 4.0F, 0.0F);

    result = resolveIntensity(
        AuthoredIntensity{-1.0F, IntensitySemantics::ArtisticRelative},
        policy);
    BAFX_CHECK(result.status == ResolveStatus::InvalidInput);

    result = resolveIntensity(
        AuthoredIntensity{
            std::numeric_limits<float>::quiet_NaN(),
            IntensitySemantics::ArtisticRelative},
        policy);
    BAFX_CHECK(result.status == ResolveStatus::InvalidInput);
}

