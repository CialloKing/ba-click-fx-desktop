#pragma once

#include "bafx/core/types.hpp"
#include "bafx/core/unity_bloom.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

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

// This status crosses renderer and diagnostics boundaries, so keep it with the
// ROI contract instead of tying performance reporting to the Windows backend.
enum class ActiveFxRoiStatus : std::uint8_t
{
    Disabled,
    AppliedPrefilter,
    NoVisualPlan,
    BloomDisabled,
    CoreMode,
    BackgroundDifferentialBloom,
    TouchesBoundary,
    AreaTooLarge,
    BenefitTooSmall,
    Context1Unavailable,
    SharedTargetFullWrite,
    RendererFallback
};

[[nodiscard]] constexpr std::string_view activeFxRoiStatusName(
    const ActiveFxRoiStatus status) noexcept
{
    switch (status)
    {
    case ActiveFxRoiStatus::Disabled:
        return "disabled";
    case ActiveFxRoiStatus::AppliedPrefilter:
        return "prefilter-roi";
    case ActiveFxRoiStatus::NoVisualPlan:
        return "no-visual-plan";
    case ActiveFxRoiStatus::BloomDisabled:
        return "bloom-disabled";
    case ActiveFxRoiStatus::CoreMode:
        return "core-mode";
    case ActiveFxRoiStatus::BackgroundDifferentialBloom:
        return "background-differential-bloom";
    case ActiveFxRoiStatus::TouchesBoundary:
        return "boundary-fallback";
    case ActiveFxRoiStatus::AreaTooLarge:
        return "area-fallback";
    case ActiveFxRoiStatus::BenefitTooSmall:
        return "benefit-too-small";
    case ActiveFxRoiStatus::Context1Unavailable:
        return "context1-unavailable";
    case ActiveFxRoiStatus::SharedTargetFullWrite:
        return "shared-target-full-write";
    case ActiveFxRoiStatus::RendererFallback:
        return "renderer-fallback";
    }
    return "renderer-fallback";
}

enum class ActiveFxRoiAdaptiveDecision : std::uint8_t
{
    Apply,
    BenefitTooSmall,
    AreaTooLarge
};

// Enter conservatively at 50%, then retain ROI until 65% to avoid switching
// paths on every frame when an animated footprint hovers near the threshold.
[[nodiscard]] ActiveFxRoiAdaptiveDecision decideActiveFxRoiAdaptivePath(
    bool previouslyActive,
    std::uint64_t candidatePixels,
    std::uint64_t fullTargetPixels) noexcept;

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

struct UnityBloomPassPixelTotals
{
    std::uint64_t fullPixels{0};
    std::uint64_t candidatePixels{0};
};

struct UnityBloomPassRoiPlan
{
    BloomRoiPlan basePlan{};
    std::array<RectI, unityBloomMaxMipCount> downRects{};
    std::array<RectI, unityBloomMaxMipCount - 1U> upRects{};
    RectI resolveRect{};
    std::uint8_t mipCount{0};
    UnityBloomPassPixelTotals prefilterPixels{};
    UnityBloomPassPixelTotals pyramidPixels{};
    UnityBloomPassPixelTotals resolvePixels{};
    UnityBloomPassPixelTotals totalPixels{};
};

struct UnityBloomPassRoiPlanResult
{
    UnityBloomPassRoiPlan plan{};
    RoiStatus status{RoiStatus::Ok};
};

[[nodiscard]] BloomRoiPlanResult planBloomRoi(
    RectI sourceSupport,
    RectI monitorBounds,
    const PyramidFootprint& footprint) noexcept;

// Build the conservative footprint used by the current Unity Bloom shader
// graph. Consumers may use it only for passes covered by their pixel
// equivalence contract; every unsupported pass retains its full-screen path.
[[nodiscard]] BloomRoiPlanResult planUnityBloomRoi(
    RectI sourceSupport,
    RectI monitorBounds,
    const UnityBloomPlan& bloomPlan) noexcept;

// Produce target-local half-open rectangles for the unchanged Unity Bloom
// viewport and mip chain. Pass rectangles retain both forward nonzero support
// and backward contributing dependencies. resolveRect comes from the full-UV
// forward chain; basePlan's legacy guard remains diagnostic only.
[[nodiscard]] UnityBloomPassRoiPlanResult planUnityBloomPassRoi(
    RectI sourceSupport,
    RectI monitorBounds,
    const UnityBloomPlan& bloomPlan) noexcept;

[[nodiscard]] RectI unite(RectI lhs, RectI rhs) noexcept;

}
