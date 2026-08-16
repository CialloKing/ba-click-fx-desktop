#pragma once

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

struct CompositionOutputState final
{
    DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
    DXGI_COLOR_SPACE_TYPE colorSpace{DXGI_COLOR_SPACE_CUSTOM};
    CompositionOutputTransfer transfer{CompositionOutputTransfer::Unknown};
    CompositionOutputFallback fallback{CompositionOutputFallback::None};
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

}
