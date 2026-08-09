#include "bafx/core/material.hpp"

#include <algorithm>
#include <cmath>

namespace bafx::core
{
namespace
{

[[nodiscard]] bool validLimit(const float value) noexcept
{
    return std::isfinite(value) && value > 0.0F;
}

[[nodiscard]] bool isFinite(const Float3 value) noexcept
{
    return std::isfinite(value.r) && std::isfinite(value.g) && std::isfinite(value.b);
}

[[nodiscard]] Float3 sanitizeVector(
    const Float3 value,
    const float upperBound,
    bool& sanitized) noexcept
{
    if (!isFinite(value))
    {
        sanitized = true;
        return Float3{};
    }

    const Float3 bounded{
        std::clamp(value.r, 0.0F, upperBound),
        std::clamp(value.g, 0.0F, upperBound),
        std::clamp(value.b, 0.0F, upperBound)};
    sanitized = sanitized
        || bounded.r != value.r
        || bounded.g != value.g
        || bounded.b != value.b;
    return bounded;
}

}

SanitizedMaterial sanitizeMaterialOutputs(
    const MaterialOutputs input,
    const MaterialLimits& limits) noexcept
{
    SanitizedMaterial result{};
    if (!validLimit(limits.maxNormal)
        || !validLimit(limits.maxDirectEmission)
        || !validLimit(limits.maxBloomSeed))
    {
        result.status = MaterialSanitizeStatus::InvalidLimits;
        return result;
    }

    if (!std::isfinite(input.alpha))
    {
        result.status = MaterialSanitizeStatus::Sanitized;
        return result;
    }

    bool sanitized = false;
    result.outputs.alpha = std::clamp(input.alpha, 0.0F, 1.0F);
    sanitized = result.outputs.alpha != input.alpha;

    // Associated-alpha color may not exceed alpha; HDR energy belongs in emission.
    const float normalUpper = std::min(result.outputs.alpha, limits.maxNormal);
    result.outputs.normalPremultiplied = sanitizeVector(
        input.normalPremultiplied,
        normalUpper,
        sanitized);
    result.outputs.directEmission = sanitizeVector(
        input.directEmission,
        limits.maxDirectEmission,
        sanitized);
    result.outputs.bloomSeed = sanitizeVector(
        input.bloomSeed,
        limits.maxBloomSeed,
        sanitized);
    result.status = sanitized
        ? MaterialSanitizeStatus::Sanitized
        : MaterialSanitizeStatus::Ok;
    return result;
}

OverlayPixel makeOverlayPixel(
    const MaterialOutputs& material,
    const Float3 bloomDelta,
    const OutputPolicy& policy) noexcept
{
    OverlayPixel pixel{material.normalPremultiplied, material.alpha};

    const bool emissionAllowed = policy.extendedPremultipliedVerified
        && !(policy.encoding == OutputEncoding::SdrDisplayRelative
            && policy.sdrEmissionBehavior == SdrEmissionBehavior::DisableExtendedEmission);
    if (!emissionAllowed)
    {
        return pixel;
    }

    Float3 safeBloom{};
    if (isFinite(bloomDelta))
    {
        safeBloom = Float3{
            std::max(bloomDelta.r, 0.0F),
            std::max(bloomDelta.g, 0.0F),
            std::max(bloomDelta.b, 0.0F)};
    }

    const Float3 emission{
        material.directEmission.r + safeBloom.r,
        material.directEmission.g + safeBloom.g,
        material.directEmission.b + safeBloom.b};
    const float visibility = policy.emissionLayerOrder == EmissionLayerOrder::BehindCoverage
        ? (1.0F - std::clamp(material.alpha, 0.0F, 1.0F))
        : 1.0F;
    pixel.premultipliedRgb.r += visibility * emission.r;
    pixel.premultipliedRgb.g += visibility * emission.g;
    pixel.premultipliedRgb.b += visibility * emission.b;
    return pixel;
}

}

