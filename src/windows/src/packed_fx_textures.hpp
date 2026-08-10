#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace bafx::windows
{

enum class PackedFxTextureId : std::uint8_t
{
    CenterDisk,
    DissolveRing,
    TriangleAtlas,
    Trail
};

enum class PackedFxTextureLayout : std::uint8_t
{
    R8Unorm,
    Rgba8Srgb
};

struct DecodedPackedFxTexture
{
    std::string_view name;
    PackedFxTextureLayout layout{PackedFxTextureLayout::Rgba8Srgb};
    std::uint32_t width{0U};
    std::uint32_t height{0U};
    std::uint32_t rowPitch{0U};
    std::string_view decodedSha256;
    std::vector<std::uint8_t> pixels;
};

// Each payload is a raw LZ4 block compiled directly into .rdata. Decoding one
// texture at a time lets the renderer release CPU pixels immediately after upload.
[[nodiscard]] DecodedPackedFxTexture decodePackedFxTexture(
    PackedFxTextureId id);

}
