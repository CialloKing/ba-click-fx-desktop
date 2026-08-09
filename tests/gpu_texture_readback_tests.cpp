#include "test_support.hpp"

#include "bafx/windows/error.hpp"
#include "bafx/windows/gpu_texture_readback.hpp"
#include "gpu_texture_readback_detail.hpp"

#include <d3d11.h>
#include <wrl/client.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace bafx::windows;

namespace
{

struct WarpDevice
{
    ComPtr<ID3D11Device> device{};
    ComPtr<ID3D11DeviceContext> context{};
};

[[nodiscard]] WarpDevice createWarpDevice()
{
    constexpr std::array featureLevels{
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0};
    WarpDevice graphics{};
    D3D_FEATURE_LEVEL selectedLevel{};
    throwIfFailed(
        D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_WARP,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            featureLevels.data(),
            static_cast<UINT>(featureLevels.size()),
            D3D11_SDK_VERSION,
            &graphics.device,
            &selectedLevel,
            &graphics.context),
        "D3D11CreateDevice(WARP readback test)");
    BAFX_CHECK(selectedLevel >= D3D_FEATURE_LEVEL_11_0);
    return graphics;
}

[[nodiscard]] ComPtr<ID3D11Texture2D> createPatternTexture(ID3D11Device* device)
{
    constexpr std::uint32_t width = 3U;
    constexpr std::uint32_t height = 3U;
    constexpr std::size_t sourceRowBytes = 32U;
    constexpr std::array<Rgba16FloatPixel, width * height> pattern{{
        {0x3C00U, 0x4000U, 0x4200U, 0x4400U},
        {0x4500U, 0x4600U, 0x4700U, 0x4800U},
        {0x4900U, 0x4A00U, 0x4B00U, 0x4C00U},
        {0x4D00U, 0x4E00U, 0x4F00U, 0x5000U},
        {0x5100U, 0x5200U, 0x5300U, 0x5400U},
        {0x5500U, 0x5600U, 0x5700U, 0x5800U},
        {0x5900U, 0x5A00U, 0x5B00U, 0x5C00U},
        {0x5D00U, 0x5E00U, 0x5F00U, 0x6000U},
        {0x6100U, 0x6200U, 0x6300U, 0x6400U}}};
    std::vector<std::uint8_t> source(sourceRowBytes * height, 0xCDU);
    const std::size_t tightRowBytes = width * sizeof(Rgba16FloatPixel);
    for (std::uint32_t y = 0U; y < height; ++y)
    {
        std::memcpy(
            source.data() + static_cast<std::size_t>(y) * sourceRowBytes,
            pattern.data() + static_cast<std::size_t>(y) * width,
            tightRowBytes);
    }

    D3D11_TEXTURE2D_DESC description{};
    description.Width = width;
    description.Height = height;
    description.MipLevels = 1U;
    description.ArraySize = 1U;
    description.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    description.SampleDesc = DXGI_SAMPLE_DESC{1U, 0U};
    description.Usage = D3D11_USAGE_DEFAULT;
    const D3D11_SUBRESOURCE_DATA initialData{
        source.data(),
        static_cast<UINT>(sourceRowBytes),
        0U};

    ComPtr<ID3D11Texture2D> texture;
    throwIfFailed(
        device->CreateTexture2D(&description, &initialData, &texture),
        "ID3D11Device::CreateTexture2D(WARP readback pattern)");
    return texture;
}

void checkPixel(
    const Rgba16FloatPixel actual,
    const Rgba16FloatPixel expected)
{
    BAFX_CHECK(actual.red == expected.red);
    BAFX_CHECK(actual.green == expected.green);
    BAFX_CHECK(actual.blue == expected.blue);
    BAFX_CHECK(actual.alpha == expected.alpha);
}

}

BAFX_TEST(half_to_float_preserves_ieee_754_binary16_boundaries)
{
    BAFX_CHECK(halfToFloat(0x0000U) == 0.0F);
    BAFX_CHECK(!std::signbit(halfToFloat(0x0000U)));
    BAFX_CHECK(halfToFloat(0x8000U) == 0.0F);
    BAFX_CHECK(std::signbit(halfToFloat(0x8000U)));
    BAFX_CHECK_NEAR(halfToFloat(0x0001U), std::ldexp(1.0F, -24), 0.0F);
    BAFX_CHECK_NEAR(halfToFloat(0x03FFU), std::ldexp(1023.0F, -24), 0.0F);
    BAFX_CHECK_NEAR(halfToFloat(0x0400U), std::ldexp(1.0F, -14), 0.0F);
    BAFX_CHECK_NEAR(halfToFloat(0x3C00U), 1.0F, 0.0F);
    BAFX_CHECK_NEAR(halfToFloat(0xC000U), -2.0F, 0.0F);
    BAFX_CHECK_NEAR(halfToFloat(0x7BFFU), 65504.0F, 0.0F);
    BAFX_CHECK(std::isinf(halfToFloat(0x7C00U)));
    BAFX_CHECK(halfToFloat(0x7C00U) > 0.0F);
    BAFX_CHECK(std::isinf(halfToFloat(0xFC00U)));
    BAFX_CHECK(halfToFloat(0xFC00U) < 0.0F);
    BAFX_CHECK(std::isnan(halfToFloat(0x7E00U)));
}

BAFX_TEST(warp_readback_preserves_channel_bits_rows_and_regions)
{
    const WarpDevice graphics = createWarpDevice();
    const ComPtr<ID3D11Texture2D> texture = createPatternTexture(graphics.device.Get());

    const Rgba16FloatImage full = readbackRgba16FloatTexture(
        graphics.context.Get(),
        texture.Get());
    BAFX_CHECK(full.width == 3U);
    BAFX_CHECK(full.height == 3U);
    BAFX_CHECK(full.pixels.size() == 9U);
    checkPixel(full.pixels[0], Rgba16FloatPixel{0x3C00U, 0x4000U, 0x4200U, 0x4400U});
    checkPixel(full.pixels[3], Rgba16FloatPixel{0x4D00U, 0x4E00U, 0x4F00U, 0x5000U});
    checkPixel(full.pixels[8], Rgba16FloatPixel{0x6100U, 0x6200U, 0x6300U, 0x6400U});

    const Rgba16FloatImage region = readbackRgba16FloatTexture(
        graphics.context.Get(),
        texture.Get(),
        TextureReadbackRegion{1U, 1U, 2U, 2U});
    BAFX_CHECK(region.width == 2U);
    BAFX_CHECK(region.height == 2U);
    BAFX_CHECK(region.pixels.size() == 4U);
    checkPixel(region.pixels[0], Rgba16FloatPixel{0x5100U, 0x5200U, 0x5300U, 0x5400U});
    checkPixel(region.pixels[1], Rgba16FloatPixel{0x5500U, 0x5600U, 0x5700U, 0x5800U});
    checkPixel(region.pixels[2], Rgba16FloatPixel{0x5D00U, 0x5E00U, 0x5F00U, 0x6000U});
    checkPixel(region.pixels[3], Rgba16FloatPixel{0x6100U, 0x6200U, 0x6300U, 0x6400U});
}

BAFX_TEST(mapped_row_copy_ignores_padding_between_fp16_rows)
{
    constexpr std::uint32_t width = 2U;
    constexpr std::uint32_t height = 3U;
    constexpr std::size_t rowPitch = 24U;
    constexpr std::array<Rgba16FloatPixel, width * height> pattern{{
        {0x3C00U, 0x4000U, 0x4200U, 0x4400U},
        {0x4500U, 0x4600U, 0x4700U, 0x4800U},
        {0x4900U, 0x4A00U, 0x4B00U, 0x4C00U},
        {0x4D00U, 0x4E00U, 0x4F00U, 0x5000U},
        {0x5100U, 0x5200U, 0x5300U, 0x5400U},
        {0x5500U, 0x5600U, 0x5700U, 0x5800U}}};
    std::array<std::uint8_t, rowPitch * height> mapped{};
    mapped.fill(0xCDU);
    const std::size_t tightRowBytes = width * sizeof(Rgba16FloatPixel);
    for (std::uint32_t y = 0U; y < height; ++y)
    {
        std::memcpy(
            mapped.data() + static_cast<std::size_t>(y) * rowPitch,
            pattern.data() + static_cast<std::size_t>(y) * width,
            tightRowBytes);
    }

    const Rgba16FloatImage image = bafx::windows::detail::copyMappedRgba16FloatRows(
        mapped.data(),
        rowPitch,
        width,
        height);
    BAFX_CHECK(image.width == width);
    BAFX_CHECK(image.height == height);
    BAFX_CHECK(image.pixels.size() == pattern.size());
    for (std::size_t index = 0U; index < pattern.size(); ++index)
    {
        checkPixel(image.pixels[index], pattern[index]);
    }
}
