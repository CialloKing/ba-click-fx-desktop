#pragma once

#include "bafx/windows/gpu_texture_readback.hpp"

#include <cstdint>
#include <filesystem>

namespace bafx::capture
{

struct LayerArtifact
{
    std::uint32_t width{0U};
    std::uint32_t height{0U};
    std::uint64_t rawBytes{0U};
};

[[nodiscard]] LayerArtifact writeLayerArtifact(
    const std::filesystem::path& directory,
    const std::filesystem::path& stem,
    const bafx::windows::Rgba16FloatImage& image);

}
