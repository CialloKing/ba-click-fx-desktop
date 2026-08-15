#pragma once

#include <dxgi1_4.h>

#include <cstdint>

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

}
