#include "test_support.hpp"

#include "bafx/core/theme_color.hpp"

#include <algorithm>
#include <cmath>
#include <string_view>

namespace
{

[[nodiscard]] float linearToSrgb(const float value) noexcept
{
    const float linear = (std::clamp)(value, 0.0F, 1.0F);
    if (linear <= 0.0031308F)
    {
        return linear * 12.92F;
    }
    return 1.055F * std::pow(linear, 1.0F / 2.4F) - 0.055F;
}

[[nodiscard]] bafx::core::Float3 srgbToLinear(
    const float red,
    const float green,
    const float blue) noexcept
{
    const auto decode = [](const float value) noexcept
    {
        if (value <= 0.04045F)
        {
            return value / 12.92F;
        }
        return std::pow((value + 0.055F) / 1.055F, 2.4F);
    };
    return bafx::core::Float3{decode(red), decode(green), decode(blue)};
}

[[nodiscard]] float maximumChannelDifference(
    const bafx::core::Float3 left,
    const bafx::core::Float3 right) noexcept
{
    return (std::max)({
        std::abs(left.r - right.r),
        std::abs(left.g - right.g),
        std::abs(left.b - right.b)});
}

}

BAFX_TEST(relative_oklch_default_theme_is_an_exact_linear_identity)
{
    const auto theme = bafx::core::createRelativeOklchTheme("#4ca7ff");
    BAFX_CHECK(theme.has_value());
    BAFX_CHECK(theme->identity);
    BAFX_CHECK(!theme->invisible);
    BAFX_CHECK_NEAR(theme->coverageScale, 1.0F, 0.0F);

    const bafx::core::Float3 source{0.12345F, 0.67891F, 1.0F};
    const bafx::core::Float3 themed = bafx::core::applyRelativeOklchTheme(
        source,
        *theme);
    BAFX_CHECK(themed.r == source.r);
    BAFX_CHECK(themed.g == source.g);
    BAFX_CHECK(themed.b == source.b);

    const auto upperCase = bafx::core::createRelativeOklchTheme("#4CA7FF");
    BAFX_CHECK(upperCase.has_value());
    BAFX_CHECK(upperCase->identity);
    const auto mixedCase = bafx::core::createRelativeOklchTheme("#4cA7fF");
    BAFX_CHECK(mixedCase.has_value());
    BAFX_CHECK(mixedCase->identity);
}

BAFX_TEST(relative_oklch_maps_the_base_blue_to_each_requested_theme)
{
    const bafx::core::Float3 baseBlue = srgbToLinear(
        76.0F / 255.0F,
        167.0F / 255.0F,
        255.0F / 255.0F);
    struct ExpectedTheme final
    {
        std::string_view color;
        bafx::core::Float3 srgb;
    };
    for (const ExpectedTheme expected : {
             ExpectedTheme{"#ff3b30", {1.0F, 59.0F / 255.0F, 48.0F / 255.0F}},
             ExpectedTheme{"#ffffff", {1.0F, 1.0F, 1.0F}},
             ExpectedTheme{"#808080", {128.0F / 255.0F, 128.0F / 255.0F, 128.0F / 255.0F}},
             ExpectedTheme{"#200002", {32.0F / 255.0F, 0.0F, 2.0F / 255.0F}}})
    {
        const auto theme = bafx::core::createRelativeOklchTheme(expected.color);
        BAFX_CHECK(theme.has_value());
        const bafx::core::Float3 mapped = bafx::core::applyRelativeOklchTheme(
            baseBlue,
            *theme);
        const bafx::core::Float3 expectedLinear = srgbToLinear(
            expected.srgb.r,
            expected.srgb.g,
            expected.srgb.b);
        BAFX_CHECK_NEAR(maximumChannelDifference(mapped, expectedLinear), 0.0F, 1.5e-4F);
    }
}

BAFX_TEST(relative_oklch_handles_neutral_black_and_near_black_themes)
{
    const bafx::core::Float3 source = srgbToLinear(
        61.0F / 255.0F,
        100.0F / 255.0F,
        1.0F);
    const auto gray = bafx::core::createRelativeOklchTheme("#808080");
    BAFX_CHECK(gray.has_value());
    const bafx::core::Float3 grayMapped = bafx::core::applyRelativeOklchTheme(
        source,
        *gray);
    BAFX_CHECK_NEAR(grayMapped.r, grayMapped.g, 1.0e-5F);
    BAFX_CHECK_NEAR(grayMapped.g, grayMapped.b, 1.0e-5F);

    const auto black = bafx::core::createRelativeOklchTheme("#000000");
    BAFX_CHECK(black.has_value());
    BAFX_CHECK(black->invisible);
    BAFX_CHECK_NEAR(black->coverageScale, 0.0F, 0.0F);
    const bafx::core::Float3 blackMapped = bafx::core::applyRelativeOklchTheme(
        source,
        *black);
    BAFX_CHECK_NEAR(maximumChannelDifference(blackMapped, {}), 0.0F, 0.0F);

    const auto nearBlack = bafx::core::createRelativeOklchTheme("#000001");
    BAFX_CHECK(nearBlack.has_value());
    BAFX_CHECK(!nearBlack->invisible);
    BAFX_CHECK_NEAR(nearBlack->coverageScale, 1.0F / 255.0F, 1.0e-7F);
    const bafx::core::Float3 nearBlackMapped = bafx::core::applyRelativeOklchTheme(
        srgbToLinear(76.0F / 255.0F, 167.0F / 255.0F, 1.0F),
        *nearBlack);
    BAFX_CHECK(linearToSrgb(nearBlackMapped.b) > 0.0F);
    BAFX_CHECK(linearToSrgb(nearBlackMapped.b) <= 1.0001F / 255.0F);
}

BAFX_TEST(relative_oklch_rejects_invalid_hex_and_maps_into_srgb_gamut)
{
    BAFX_CHECK(!bafx::core::createRelativeOklchTheme("red").has_value());
    BAFX_CHECK(!bafx::core::createRelativeOklchTheme("#00000000").has_value());

    for (const std::string_view color : {
             "#ff0000",
             "#00ff00",
             "#0000ff",
             "#ff00ff",
             "#00ffff"})
    {
        const auto theme = bafx::core::createRelativeOklchTheme(color);
        BAFX_CHECK(theme.has_value());
        for (const bafx::core::Float3 source : {
                 srgbToLinear(1.0F, 0.0F, 0.0F),
                 srgbToLinear(0.0F, 1.0F, 0.0F),
                 srgbToLinear(0.0F, 0.0F, 1.0F),
                 srgbToLinear(1.0F, 1.0F, 1.0F),
                 srgbToLinear(7.0F / 255.0F, 19.0F / 255.0F, 83.0F / 255.0F)})
        {
            const bafx::core::Float3 mapped = bafx::core::applyRelativeOklchTheme(
                source,
                *theme);
            BAFX_CHECK(std::isfinite(mapped.r));
            BAFX_CHECK(std::isfinite(mapped.g));
            BAFX_CHECK(std::isfinite(mapped.b));
            BAFX_CHECK(mapped.r >= 0.0F && mapped.r <= 1.0F);
            BAFX_CHECK(mapped.g >= 0.0F && mapped.g <= 1.0F);
            BAFX_CHECK(mapped.b >= 0.0F && mapped.b <= 1.0F);
        }
    }
}
