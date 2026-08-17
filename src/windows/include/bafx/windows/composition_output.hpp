#pragma once

#include "bafx/core/intensity.hpp"

#include <dxgi1_4.h>

#include <cstdint>
#include <optional>

namespace bafx::windows
{

enum class CompositionOutputTransfer : std::uint8_t
{
    Unknown,
    LinearScRgb,
    SdrGamma22
};

enum class CompositionOutputPreference : std::uint8_t
{
    ConservativeSdr,
    PreferLinearScRgb
};

enum class CompositionOutputFallback : std::uint8_t
{
    None,
    ConservativeSdr
};

enum class CompositionOutputMappingMode : std::uint8_t
{
    ConservativeSdr,
    AdvancedColorScRgb,
    HdrSceneReferredScRgb
};

struct CompositionOutputMapping final
{
    CompositionOutputMappingMode mode{
        CompositionOutputMappingMode::ConservativeSdr};
    // Unity material values remain artistic throughout the FP16 pipeline.
    // Reference white is output metadata, not an implicit authoring scale.
    bafx::core::IntensitySemantics intensitySemantics{
        bafx::core::IntensitySemantics::ArtisticRelative};
    // Final-output white is intentionally independent from the WGC capture
    // white. A conservative SDR swap chain still needs the latter to convert
    // physical scRGB background pixels into Unity's relative working space.
    float referenceWhiteNits{0.0F};
    bool referenceWhiteValid{false};
    float backgroundReferenceWhiteNits{0.0F};
    bool backgroundReferenceWhiteValid{false};
    // HDR/WCG WGC pixels are physical scRGB. When this bit is set, an unknown
    // white level must disable background participation instead of assuming
    // the legacy one-unit reference white.
    bool backgroundReferenceWhiteRequired{false};

    [[nodiscard]] bool operator==(
        const CompositionOutputMapping&) const noexcept = default;
};

struct CompositionOutputPolicy final
{
    CompositionOutputPreference preference{
        CompositionOutputPreference::ConservativeSdr};
    CompositionOutputMapping mapping{};

    [[nodiscard]] bool operator==(
        const CompositionOutputPolicy&) const noexcept = default;
};

[[nodiscard]] constexpr CompositionOutputPolicy compositionOutputPolicyFor(
    const CompositionOutputPreference preference) noexcept
{
    CompositionOutputPolicy policy{};
    policy.preference = preference;
    if (preference == CompositionOutputPreference::PreferLinearScRgb)
    {
        // Callers without monitor metadata still need an explicit linear
        // mapping. Production display sessions replace this with a per-screen
        // HDR or Advanced Color policy before presenting.
        policy.mapping.mode = CompositionOutputMappingMode::AdvancedColorScRgb;
    }
    return policy;
}

struct CompositionOutputState final
{
    DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
    DXGI_COLOR_SPACE_TYPE colorSpace{DXGI_COLOR_SPACE_CUSTOM};
    CompositionOutputTransfer transfer{CompositionOutputTransfer::Unknown};
    CompositionOutputFallback fallback{CompositionOutputFallback::None};
    // Keep the failed requested transport operation even when the fallback
    // swap chain is created successfully, so diagnostics identify the cause.
    HRESULT fallbackResult{S_OK};
    CompositionOutputMapping mapping{};
    bool extendedPremultiplied{false};

    [[nodiscard]] bool operator==(
        const CompositionOutputState&) const noexcept = default;
};

[[nodiscard]] constexpr std::optional<CompositionOutputPreference>
effectiveCompositionOutputPreference(
    const CompositionOutputState& output) noexcept
{
    switch (output.transfer)
    {
    case CompositionOutputTransfer::LinearScRgb:
        return CompositionOutputPreference::PreferLinearScRgb;
    case CompositionOutputTransfer::SdrGamma22:
        return CompositionOutputPreference::ConservativeSdr;
    case CompositionOutputTransfer::Unknown:
        return std::nullopt;
    }
    return std::nullopt;
}

[[nodiscard]] constexpr bool compositionOutputSatisfiesPreference(
    const CompositionOutputState& output,
    const CompositionOutputPreference preference) noexcept
{
    const std::optional<CompositionOutputPreference> effective =
        effectiveCompositionOutputPreference(output);
    // The fallback marker explains how SDR was selected; the transfer remains
    // the authoritative fact when comparing against the current policy.
    return effective.has_value() && *effective == preference;
}

[[nodiscard]] constexpr bool compositionOutputSatisfiesPolicy(
    const CompositionOutputState& output,
    const CompositionOutputPolicy& policy) noexcept
{
    return compositionOutputSatisfiesPreference(output, policy.preference)
        && output.mapping == policy.mapping;
}

}
