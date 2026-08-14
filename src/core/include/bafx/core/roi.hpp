#pragma once

#include "bafx/core/types.hpp"
#include "bafx/core/unity_bloom.hpp"

#include <cstdint>
#include <span>

namespace bafx::core
{

struct ReceptiveFieldTerm
{
    std::uint8_t mipLevel{0};
    std::uint16_t radiusX{0};
    std::uint16_t radiusY{0};
};

struct PyramidFootprint
{
    // The caller supplies the longest dependency path, including sampling footprints.
    std::span<const ReceptiveFieldTerm> worstPath{};
    std::uint8_t coarsestMipLevel{0};
};

enum class RoiStatus : std::uint8_t
{
    Ok,
    Empty,
    InvalidRect,
    InvalidFootprint,
    IntegerOverflow
};

struct BloomRoiPlan
{
    std::uint32_t guardX{0};
    std::uint32_t guardY{0};
    std::uint32_t phasePeriod{1};
    RectI sourceSupport{};
    RectI bloomOutput{};
    RectI alignedWork{};
};

struct BloomRoiPlanResult
{
    BloomRoiPlan plan{};
    RoiStatus status{RoiStatus::Ok};
};

[[nodiscard]] BloomRoiPlanResult planBloomRoi(
    RectI sourceSupport,
    RectI monitorBounds,
    const PyramidFootprint& footprint) noexcept;

// Build the conservative footprint used by the current Unity Bloom shader
// graph. The result is only a plan; the renderer must keep the full-screen
// path until ROI pixel equivalence has been accepted by ADR-006.
[[nodiscard]] BloomRoiPlanResult planUnityBloomRoi(
    RectI sourceSupport,
    RectI monitorBounds,
    const UnityBloomPlan& bloomPlan) noexcept;

[[nodiscard]] RectI unite(RectI lhs, RectI rhs) noexcept;

}
