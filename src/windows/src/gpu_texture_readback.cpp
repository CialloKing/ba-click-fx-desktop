#include "bafx/windows/gpu_texture_readback.hpp"

#include "bafx/windows/error.hpp"
#include "gpu_texture_readback_detail.hpp"

#include <wrl/client.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace bafx::windows
{
namespace
{

using Microsoft::WRL::ComPtr;

static_assert(sizeof(Rgba16FloatPixel) == sizeof(std::uint16_t) * 4U);
static_assert(std::is_trivially_copyable_v<Rgba16FloatPixel>);
static_assert(sizeof(Bgra8UnormPixel) == sizeof(std::uint8_t) * 4U);
static_assert(std::is_trivially_copyable_v<Bgra8UnormPixel>);

void validateArguments(
    ID3D11DeviceContext* context,
    ID3D11Texture2D* source,
    const D3D11_TEXTURE2D_DESC& description,
    const TextureReadbackRegion region)
{
    if (context == nullptr || source == nullptr)
    {
        throw std::invalid_argument("FP16 texture readback requires a context and source");
    }
    if (description.Format != DXGI_FORMAT_R16G16B16A16_FLOAT)
    {
        throw std::invalid_argument("FP16 texture readback requires RGBA16_FLOAT");
    }
    if (description.SampleDesc.Count != 1U)
    {
        throw std::invalid_argument("FP16 texture readback does not resolve multisampling");
    }
    if (region.width == 0U || region.height == 0U
        || region.left >= description.Width
        || region.top >= description.Height
        || region.width > description.Width - region.left
        || region.height > description.Height - region.top)
    {
        throw std::out_of_range("FP16 texture readback region is outside the source");
    }

    ComPtr<ID3D11Device> sourceDevice;
    ComPtr<ID3D11Device> contextDevice;
    source->GetDevice(&sourceDevice);
    context->GetDevice(&contextDevice);
    if (sourceDevice.Get() != contextDevice.Get())
    {
        throw std::invalid_argument("FP16 texture and context belong to different devices");
    }
}

[[nodiscard]] ComPtr<ID3D11Texture2D> createStagingTexture(
    ID3D11Device* device,
    const TextureReadbackRegion region)
{
    D3D11_TEXTURE2D_DESC description{};
    description.Width = region.width;
    description.Height = region.height;
    description.MipLevels = 1U;
    description.ArraySize = 1U;
    description.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    description.SampleDesc = DXGI_SAMPLE_DESC{1U, 0U};
    description.Usage = D3D11_USAGE_STAGING;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    ComPtr<ID3D11Texture2D> staging;
    throwIfFailed(
        device->CreateTexture2D(&description, nullptr, &staging),
        "ID3D11Device::CreateTexture2D(FP16 readback staging)");
    return staging;
}

[[nodiscard]] std::size_t checkedPixelCount(
    const std::uint32_t width,
    const std::uint32_t height)
{
    const std::size_t widthValue = width;
    const std::size_t heightValue = height;
    if (width == 0U || height == 0U)
    {
        throw std::invalid_argument("FP16 mapped image dimensions must be nonzero");
    }
    if (heightValue > std::numeric_limits<std::size_t>::max() / widthValue)
    {
        throw std::overflow_error("FP16 texture readback pixel count exceeds address space");
    }
    return widthValue * heightValue;
}

}

namespace detail
{

Rgba16FloatImage copyMappedRgba16FloatRows(
    const void* data,
    const std::size_t rowPitch,
    const std::uint32_t width,
    const std::uint32_t height)
{
    if (data == nullptr)
    {
        throw std::invalid_argument("FP16 mapped image requires source data");
    }

    const std::size_t tightRowBytes = static_cast<std::size_t>(width)
        * sizeof(Rgba16FloatPixel);
    if (rowPitch < tightRowBytes)
    {
        throw std::invalid_argument("FP16 mapped row pitch is smaller than one row");
    }

    Rgba16FloatImage image{
        width,
        height,
        std::vector<Rgba16FloatPixel>(checkedPixelCount(width, height))};
    for (std::uint32_t y = 0U; y < height; ++y)
    {
        const auto* sourceRow = static_cast<const std::uint8_t*>(data)
            + static_cast<std::size_t>(y) * rowPitch;
        Rgba16FloatPixel* destinationRow = image.pixels.data()
            + static_cast<std::size_t>(y) * width;
        // Copy each mapped row independently because D3D may add driver-specific padding.
        std::memcpy(destinationRow, sourceRow, tightRowBytes);
    }
    return image;
}

}

float halfToFloat(const std::uint16_t value) noexcept
{
    const bool negative = (value & 0x8000U) != 0U;
    const std::uint16_t exponent = static_cast<std::uint16_t>(
        (value >> 10U) & 0x1FU);
    const std::uint16_t mantissa = static_cast<std::uint16_t>(value & 0x03FFU);
    float result = 0.0F;
    if (exponent == 0U)
    {
        result = std::ldexp(static_cast<float>(mantissa), -24);
    }
    else if (exponent == 0x1FU)
    {
        result = mantissa == 0U
            ? std::numeric_limits<float>::infinity()
            : std::numeric_limits<float>::quiet_NaN();
    }
    else
    {
        result = std::ldexp(
            1.0F + static_cast<float>(mantissa) / 1024.0F,
            static_cast<int>(exponent) - 15);
    }
    return negative ? -result : result;
}

Rgba16FloatImage readbackRgba16FloatTexture(
    ID3D11DeviceContext* context,
    ID3D11Texture2D* source)
{
    if (source == nullptr)
    {
        throw std::invalid_argument("FP16 texture readback requires a source");
    }

    D3D11_TEXTURE2D_DESC description{};
    source->GetDesc(&description);
    return readbackRgba16FloatTexture(
        context,
        source,
        TextureReadbackRegion{0U, 0U, description.Width, description.Height});
}

Rgba16FloatImage readbackRgba16FloatTexture(
    ID3D11DeviceContext* context,
    ID3D11Texture2D* source,
    const TextureReadbackRegion region)
{
    if (source == nullptr)
    {
        throw std::invalid_argument("FP16 texture readback requires a source");
    }

    D3D11_TEXTURE2D_DESC sourceDescription{};
    source->GetDesc(&sourceDescription);
    validateArguments(context, source, sourceDescription, region);

    ComPtr<ID3D11Device> device;
    source->GetDevice(&device);
    const ComPtr<ID3D11Texture2D> staging = createStagingTexture(
        device.Get(),
        region);
    const D3D11_BOX sourceBox{
        region.left,
        region.top,
        0U,
        region.left + region.width,
        region.top + region.height,
        1U};
    context->CopySubresourceRegion(
        staging.Get(),
        0U,
        0U,
        0U,
        0U,
        source,
        0U,
        &sourceBox);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    throwIfFailed(
        context->Map(staging.Get(), 0U, D3D11_MAP_READ, 0U, &mapped),
        "ID3D11DeviceContext::Map(FP16 readback staging)");

    Rgba16FloatImage image{};
    try
    {
        image = detail::copyMappedRgba16FloatRows(
            mapped.pData,
            mapped.RowPitch,
            region.width,
            region.height);
    }
    catch (...)
    {
        context->Unmap(staging.Get(), 0U);
        throw;
    }
    context->Unmap(staging.Get(), 0U);
    return image;
}

Bgra8UnormPixel readbackBgra8UnormPixel(
    ID3D11DeviceContext* context,
    ID3D11Texture2D* source,
    const std::uint32_t x,
    const std::uint32_t y)
{
    if (context == nullptr || source == nullptr)
    {
        throw std::invalid_argument(
            "BGRA8 texture readback requires a context and source");
    }

    D3D11_TEXTURE2D_DESC sourceDescription{};
    source->GetDesc(&sourceDescription);
    if (sourceDescription.Format != DXGI_FORMAT_B8G8R8A8_UNORM)
    {
        throw std::invalid_argument(
            "BGRA8 texture readback requires B8G8R8A8_UNORM");
    }
    if (sourceDescription.SampleDesc.Count != 1U)
    {
        throw std::invalid_argument(
            "BGRA8 texture readback does not resolve multisampling");
    }
    if (x >= sourceDescription.Width || y >= sourceDescription.Height)
    {
        throw std::out_of_range(
            "BGRA8 texture readback coordinate is outside the source");
    }

    ComPtr<ID3D11Device> sourceDevice;
    ComPtr<ID3D11Device> contextDevice;
    source->GetDevice(&sourceDevice);
    context->GetDevice(&contextDevice);
    if (sourceDevice.Get() != contextDevice.Get())
    {
        throw std::invalid_argument(
            "BGRA8 texture and context belong to different devices");
    }

    D3D11_TEXTURE2D_DESC stagingDescription{};
    stagingDescription.Width = 1U;
    stagingDescription.Height = 1U;
    stagingDescription.MipLevels = 1U;
    stagingDescription.ArraySize = 1U;
    stagingDescription.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    stagingDescription.SampleDesc = DXGI_SAMPLE_DESC{1U, 0U};
    stagingDescription.Usage = D3D11_USAGE_STAGING;
    stagingDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    ComPtr<ID3D11Texture2D> staging;
    throwIfFailed(
        sourceDevice->CreateTexture2D(
            &stagingDescription,
            nullptr,
            &staging),
        "ID3D11Device::CreateTexture2D(BGRA8 pixel readback staging)");

    const D3D11_BOX sourceBox{x, y, 0U, x + 1U, y + 1U, 1U};
    context->CopySubresourceRegion(
        staging.Get(),
        0U,
        0U,
        0U,
        0U,
        source,
        0U,
        &sourceBox);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    throwIfFailed(
        context->Map(staging.Get(), 0U, D3D11_MAP_READ, 0U, &mapped),
        "ID3D11DeviceContext::Map(BGRA8 pixel readback staging)");

    Bgra8UnormPixel pixel{};
    try
    {
        if (mapped.pData == nullptr
            || mapped.RowPitch < sizeof(Bgra8UnormPixel))
        {
            throw std::runtime_error(
                "BGRA8 mapped row cannot contain one pixel");
        }
        std::memcpy(&pixel, mapped.pData, sizeof(pixel));
    }
    catch (...)
    {
        context->Unmap(staging.Get(), 0U);
        throw;
    }
    context->Unmap(staging.Get(), 0U);
    return pixel;
}

}
