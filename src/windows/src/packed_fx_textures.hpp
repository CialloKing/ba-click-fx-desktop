#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

namespace bafx::windows
{

enum class PackedFxTextureId : std::uint8_t
{
    CenterDisk,
    DissolveRing,
    TriangleAtlas,
    Trail
};

struct DecodedPackedFxTexture
{
    std::string_view name;
    std::uint32_t width{0U};
    std::uint32_t height{0U};
    std::uint32_t rowPitch{0U};
    std::string_view decodedSha256;
    std::size_t pixelByteCount{0U};
    std::unique_ptr<std::uint8_t[]> pixels;
};

// Each payload is a raw LZ4 block compiled directly into .rdata. Decoding one
// texture at a time lets the renderer release CPU pixels immediately after upload.
[[nodiscard]] DecodedPackedFxTexture decodePackedFxTexture(
    PackedFxTextureId id);

}
