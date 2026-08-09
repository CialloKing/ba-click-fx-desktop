#pragma once

#include <d3d11.h>

#include <cstdint>
#include <vector>

namespace bafx::windows
{

// Keep binary16 payloads untouched so HDR and extended-premultiplied values
// remain available for later numerical comparison.
struct Rgba16FloatPixel
{
    std::uint16_t red{0U};
    std::uint16_t green{0U};
    std::uint16_t blue{0U};
    std::uint16_t alpha{0U};
};

struct Rgba16FloatImage
{
    std::uint32_t width{0U};
    std::uint32_t height{0U};
    std::vector<Rgba16FloatPixel> pixels{};
};

struct TextureReadbackRegion
{
    std::uint32_t left{0U};
    std::uint32_t top{0U};
    std::uint32_t width{0U};
    std::uint32_t height{0U};
};

[[nodiscard]] float halfToFloat(std::uint16_t value) noexcept;

[[nodiscard]] Rgba16FloatImage readbackRgba16FloatTexture(
    ID3D11DeviceContext* context,
    ID3D11Texture2D* source);

[[nodiscard]] Rgba16FloatImage readbackRgba16FloatTexture(
    ID3D11DeviceContext* context,
    ID3D11Texture2D* source,
    TextureReadbackRegion region);

}
