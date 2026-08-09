#pragma once

#include "bafx/core/intensity.hpp"
#include "bafx/core/types.hpp"

#include <cstdint>

namespace bafx::core
{

struct MaterialOutputs
{
    Float3 normalPremultiplied{};
    float alpha{0.0F};
    Float3 directEmission{};
    Float3 bloomSeed{};
};

struct MaterialLimits
{
    float maxNormal{1.0F};
    float maxDirectEmission{16.0F};
    float maxBloomSeed{16.0F};
};

enum class MaterialSanitizeStatus : std::uint8_t
{
    Ok,
    Sanitized,
    InvalidLimits
};

struct SanitizedMaterial
{
    MaterialOutputs outputs{};
    MaterialSanitizeStatus status{MaterialSanitizeStatus::Ok};
};

[[nodiscard]] SanitizedMaterial sanitizeMaterialOutputs(
    MaterialOutputs input,
    const MaterialLimits& limits) noexcept;

struct OverlayPixel
{
    Float3 premultipliedRgb{};
    float alpha{0.0F};
};

[[nodiscard]] OverlayPixel makeOverlayPixel(
    const MaterialOutputs& material,
    Float3 bloomDelta,
    const OutputPolicy& policy) noexcept;

}

