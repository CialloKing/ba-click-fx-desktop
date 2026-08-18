#include "bafx/core/theme_color.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace bafx::core
{
namespace
{

constexpr float achromaticEpsilon = 1.0e-5F;
constexpr float blackEpsilon = 1.0e-7F;
constexpr float gamutEpsilon = 1.0e-7F;
constexpr std::size_t gamutSearchSteps = 28U;
constexpr float fullTurnRadians = 6.28318530717958647692F;

struct Oklab final
{
    float lightness{0.0F};
    float a{0.0F};
    float b{0.0F};
};

struct Oklch final
{
    float lightness{0.0F};
    float chroma{0.0F};
    float hueRadians{0.0F};
};

[[nodiscard]] float clampUnit(const float value) noexcept
{
    return (std::clamp)(value, 0.0F, 1.0F);
}

[[nodiscard]] float decodeSrgbChannel(const float value) noexcept
{
    const float srgb = clampUnit(value);
    if (srgb <= 0.04045F)
    {
        return srgb / 12.92F;
    }
    return std::pow((srgb + 0.055F) / 1.055F, 2.4F);
}

[[nodiscard]] float encodeSrgbChannel(const float value) noexcept
{
    const float linear = clampUnit(value);
    if (linear <= 0.0031308F)
    {
        return linear * 12.92F;
    }
    return 1.055F * std::pow(linear, 1.0F / 2.4F) - 0.055F;
}

[[nodiscard]] Oklab linearSrgbToOklab(const Float3 color) noexcept
{
    const float l = std::cbrt(
        0.4122214708F * color.r
        + 0.5363325363F * color.g
        + 0.0514459929F * color.b);
    const float m = std::cbrt(
        0.2119034982F * color.r
        + 0.6806995451F * color.g
        + 0.1073969566F * color.b);
    const float s = std::cbrt(
        0.0883024619F * color.r
        + 0.2817188376F * color.g
        + 0.6299787005F * color.b);
    return Oklab{
        0.2104542553F * l + 0.7936177850F * m - 0.0040720468F * s,
        1.9779984951F * l - 2.4285922050F * m + 0.4505937099F * s,
        0.0259040371F * l + 0.7827717662F * m - 0.8086757660F * s};
}

[[nodiscard]] Float3 oklabToLinearSrgb(const Oklab color) noexcept
{
    const float l = color.lightness + 0.3963377774F * color.a
        + 0.2158037573F * color.b;
    const float m = color.lightness - 0.1055613458F * color.a
        - 0.0638541728F * color.b;
    const float s = color.lightness - 0.0894841775F * color.a
        - 1.2914855480F * color.b;
    const float lCubed = l * l * l;
    const float mCubed = m * m * m;
    const float sCubed = s * s * s;
    return Float3{
        4.0767416621F * lCubed - 3.3077115913F * mCubed
            + 0.2309699292F * sCubed,
        -1.2684380046F * lCubed + 2.6097574011F * mCubed
            - 0.3413193965F * sCubed,
        -0.0041960863F * lCubed - 0.7034186147F * mCubed
            + 1.7076147010F * sCubed};
}

[[nodiscard]] Oklch linearSrgbToOklch(const Float3 color) noexcept
{
    const Oklab lab = linearSrgbToOklab(color);
    const float chroma = std::hypot(lab.a, lab.b);
    return Oklch{
        lab.lightness,
        chroma,
        chroma <= achromaticEpsilon ? 0.0F : std::atan2(lab.b, lab.a)};
}

[[nodiscard]] float normalizeHue(const float hueRadians) noexcept
{
    return hueRadians - std::floor(hueRadians / fullTurnRadians)
        * fullTurnRadians;
}

[[nodiscard]] Float3 oklchToLinearSrgb(
    const float lightness,
    const float chroma,
    const float hueRadians) noexcept
{
    return oklabToLinearSrgb(Oklab{
        lightness,
        chroma * std::cos(hueRadians),
        chroma * std::sin(hueRadians)});
}

[[nodiscard]] bool isInSrgbGamut(const Float3 color) noexcept
{
    return std::isfinite(color.r)
        && std::isfinite(color.g)
        && std::isfinite(color.b)
        && color.r >= -gamutEpsilon
        && color.g >= -gamutEpsilon
        && color.b >= -gamutEpsilon
        && color.r <= 1.0F + gamutEpsilon
        && color.g <= 1.0F + gamutEpsilon
        && color.b <= 1.0F + gamutEpsilon;
}

[[nodiscard]] Float3 gamutMapOklch(
    const float lightness,
    const float chroma,
    const float hueRadians) noexcept
{
    float mappedChroma = (std::max)(0.0F, chroma);
    Float3 linear = oklchToLinearSrgb(lightness, mappedChroma, hueRadians);
    if (!isInSrgbGamut(linear))
    {
        float lower = 0.0F;
        float upper = mappedChroma;
        for (std::size_t index = 0U; index < gamutSearchSteps; ++index)
        {
            const float candidate = (lower + upper) * 0.5F;
            const Float3 candidateLinear = oklchToLinearSrgb(
                lightness,
                candidate,
                hueRadians);
            if (isInSrgbGamut(candidateLinear))
            {
                lower = candidate;
            }
            else
            {
                upper = candidate;
            }
        }
        linear = oklchToLinearSrgb(lightness, lower, hueRadians);
    }
    return Float3{
        clampUnit(linear.r),
        clampUnit(linear.g),
        clampUnit(linear.b)};
}

[[nodiscard]] std::optional<std::array<std::uint8_t, 3U>> parseThemeColor(
    const std::string_view value) noexcept
{
    if (value.size() != 7U || value.front() != '#')
    {
        return std::nullopt;
    }
    const auto nibble = [](const char character) -> std::optional<std::uint8_t>
    {
        if (character >= '0' && character <= '9')
        {
            return static_cast<std::uint8_t>(character - '0');
        }
        if (character >= 'a' && character <= 'f')
        {
            return static_cast<std::uint8_t>(character - 'a' + 10);
        }
        if (character >= 'A' && character <= 'F')
        {
            return static_cast<std::uint8_t>(character - 'A' + 10);
        }
        return std::nullopt;
    };

    std::array<std::uint8_t, 3U> rgb{};
    for (std::size_t index = 0U; index < rgb.size(); ++index)
    {
        const std::optional<std::uint8_t> high = nibble(value[index * 2U + 1U]);
        const std::optional<std::uint8_t> low = nibble(value[index * 2U + 2U]);
        if (!high.has_value() || !low.has_value())
        {
            return std::nullopt;
        }
        rgb[index] = static_cast<std::uint8_t>((*high << 4U) | *low);
    }
    return rgb;
}

[[nodiscard]] Float3 srgbBytesToLinear(
    const std::array<std::uint8_t, 3U>& color) noexcept
{
    return Float3{
        decodeSrgbChannel(static_cast<float>(color[0]) / 255.0F),
        decodeSrgbChannel(static_cast<float>(color[1]) / 255.0F),
        decodeSrgbChannel(static_cast<float>(color[2]) / 255.0F)};
}

[[nodiscard]] float mapRelativeLightness(
    const float sourceLightness,
    const float targetLightness,
    const float baseLightness) noexcept
{
    if (targetLightness <= baseLightness || sourceLightness <= baseLightness)
    {
        return sourceLightness * targetLightness / baseLightness;
    }
    return targetLightness + (sourceLightness - baseLightness)
        * (1.0F - targetLightness) / (1.0F - baseLightness);
}

[[nodiscard]] const Oklch& baseThemeOklch() noexcept
{
    static const Oklch value = linearSrgbToOklch(Float3{
        decodeSrgbChannel(76.0F / 255.0F),
        decodeSrgbChannel(167.0F / 255.0F),
        decodeSrgbChannel(255.0F / 255.0F)});
    return value;
}

}

std::optional<RelativeOklchTheme> createRelativeOklchTheme(
    const std::string_view themeColor) noexcept
{
    const std::optional<std::array<std::uint8_t, 3U>> rgb = parseThemeColor(
        themeColor);
    if (!rgb.has_value())
    {
        return std::nullopt;
    }
    const Oklch target = linearSrgbToOklch(srgbBytesToLinear(*rgb));
    const Oklch& base = baseThemeOklch();
    const bool achromatic = target.chroma <= achromaticEpsilon;
    // Compare parsed bytes so every valid casing of the shipped blue reaches
    // the strict identity path instead of introducing a rounding difference.
    const bool identity = (*rgb)[0] == 76U
        && (*rgb)[1] == 167U
        && (*rgb)[2] == 255U;
    const std::uint8_t maximumChannel = (std::max)(
        (*rgb)[0],
        (std::max)((*rgb)[1], (*rgb)[2]));
    return RelativeOklchTheme{
        identity,
        target.lightness <= blackEpsilon,
        static_cast<float>(maximumChannel) / 255.0F,
        target.lightness,
        achromatic ? 0.0F : target.chroma / base.chroma,
        achromatic ? 0.0F : normalizeHue(target.hueRadians - base.hueRadians)};
}

Float3 applyRelativeOklchTheme(
    const Float3 linearColor,
    const RelativeOklchTheme& theme) noexcept
{
    if (theme.identity)
    {
        return linearColor;
    }
    if (theme.invisible)
    {
        return {};
    }

    const Oklch source = linearSrgbToOklch(Float3{
        clampUnit(linearColor.r),
        clampUnit(linearColor.g),
        clampUnit(linearColor.b)});
    if (source.lightness <= blackEpsilon)
    {
        return {};
    }
    const Oklch& base = baseThemeOklch();
    const float chroma = source.chroma <= achromaticEpsilon
        ? 0.0F
        : source.chroma * theme.chromaScale;
    return gamutMapOklch(
        mapRelativeLightness(
            source.lightness,
            theme.targetLightness,
            base.lightness),
        chroma,
        normalizeHue(source.hueRadians + theme.hueShiftRadians));
}

}
