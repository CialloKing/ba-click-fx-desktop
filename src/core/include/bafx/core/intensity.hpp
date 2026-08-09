#pragma once

#include <cstdint>

namespace bafx::core
{

enum class IntensitySemantics : std::uint8_t
{
    ArtisticRelative,
    ReferenceWhiteRelative,
    AbsoluteNits
};

struct AuthoredIntensity
{
    float value{0.0F};
    IntensitySemantics semantics{IntensitySemantics::ArtisticRelative};
};

enum class OutputEncoding : std::uint8_t
{
    SdrDisplayRelative,
    HdrSceneReferredScRgb
};

enum class AbsoluteNitsOnSdr : std::uint8_t
{
    Disable,
    UseCalibratedPeak
};

enum class EmissionLayerOrder : std::uint8_t
{
    AfterCoverage,
    BehindCoverage
};

enum class SdrEmissionBehavior : std::uint8_t
{
    AllowSaturation,
    DisableExtendedEmission
};

enum class BackgroundFallback : std::uint8_t
{
    FxOnlyBloom,
    NoBloom
};

enum class OutputPolicyKind : std::uint8_t
{
    PreserveDesktop,
    PreserveHueWithHeadroom,
    HighVisibility
};

struct OutputPolicy
{
    OutputEncoding encoding{OutputEncoding::SdrDisplayRelative};
    AbsoluteNitsOnSdr absoluteNitsOnSdr{AbsoluteNitsOnSdr::Disable};
    EmissionLayerOrder emissionLayerOrder{EmissionLayerOrder::AfterCoverage};
    SdrEmissionBehavior sdrEmissionBehavior{SdrEmissionBehavior::AllowSaturation};
    BackgroundFallback staleBackgroundFallback{BackgroundFallback::FxOnlyBloom};
    OutputPolicyKind kind{OutputPolicyKind::PreserveDesktop};

    // This calibration is dimensionless and must never be presented as nits.
    float artisticToCanonical{1.0F};
    float hdrSdrWhiteNits{80.0F};
    float calibratedSdrPeakNits{0.0F};
    float maxCanonicalIntensity{16.0F};

    // The production path keeps additive energy disabled until SPK-001 supplies evidence.
    bool extendedPremultipliedVerified{false};
};

enum class ResolveStatus : std::uint8_t
{
    Ok,
    UnsupportedOnOutput,
    InvalidInput,
    InvalidPolicy
};

struct ResolvedIntensity
{
    float canonicalValue{0.0F};
    ResolveStatus status{ResolveStatus::Ok};
    bool clamped{false};
    bool approximate{false};
};

[[nodiscard]] ResolvedIntensity resolveIntensity(
    AuthoredIntensity intensity,
    const OutputPolicy& policy) noexcept;

}

