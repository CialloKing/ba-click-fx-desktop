#include "test_support.hpp"

#include "bafx/windows/error.hpp"
#include "bafx/windows/fx_gpu_renderer.hpp"

#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace bafx::windows;

namespace
{

constexpr WindowSize testSize{256U, 256U};
constexpr float negativeTolerance = -1.0e-6F;

class ComApartment final
{
public:
    ComApartment()
    {
        const HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(result))
        {
            throw HResultError(result, "CoInitializeEx");
        }
        initialized_ = true;
    }

    ~ComApartment()
    {
        if (initialized_)
        {
            CoUninitialize();
        }
    }

    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;

private:
    bool initialized_{false};
};

struct ReadbackPixel
{
    float red{0.0F};
    float green{0.0F};
    float blue{0.0F};
    float alpha{0.0F};
};

struct RenderTarget
{
    ComPtr<ID3D11Texture2D> texture{};
    ComPtr<ID3D11RenderTargetView> view{};
};

struct WarpDevice
{
    ComPtr<ID3D11Device> device{};
    ComPtr<ID3D11DeviceContext> context{};
    D3D_FEATURE_LEVEL featureLevel{D3D_FEATURE_LEVEL_11_0};
};

[[nodiscard]] float halfToFloat(const std::uint16_t value) noexcept
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

[[nodiscard]] RenderTarget createRenderTarget(ID3D11Device* device)
{
    D3D11_TEXTURE2D_DESC description{};
    description.Width = testSize.width;
    description.Height = testSize.height;
    description.MipLevels = 1U;
    description.ArraySize = 1U;
    description.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    description.SampleDesc = DXGI_SAMPLE_DESC{1U, 0U};
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_RENDER_TARGET;

    RenderTarget target{};
    throwIfFailed(
        device->CreateTexture2D(&description, nullptr, &target.texture),
        "ID3D11Device::CreateTexture2D(WARP destination)");
    throwIfFailed(
        device->CreateRenderTargetView(target.texture.Get(), nullptr, &target.view),
        "ID3D11Device::CreateRenderTargetView(WARP destination)");
    return target;
}

[[nodiscard]] WarpDevice createWarpDevice()
{
    constexpr std::array featureLevels{
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0};
    WarpDevice graphics{};
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
            &graphics.featureLevel,
            &graphics.context),
        "D3D11CreateDevice(WARP)");
    return graphics;
}

[[nodiscard]] ComPtr<ID3D11Texture2D> createStagingTexture(ID3D11Device* device)
{
    D3D11_TEXTURE2D_DESC description{};
    description.Width = testSize.width;
    description.Height = testSize.height;
    description.MipLevels = 1U;
    description.ArraySize = 1U;
    description.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    description.SampleDesc = DXGI_SAMPLE_DESC{1U, 0U};
    description.Usage = D3D11_USAGE_STAGING;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    ComPtr<ID3D11Texture2D> texture;
    throwIfFailed(
        device->CreateTexture2D(&description, nullptr, &texture),
        "ID3D11Device::CreateTexture2D(WARP staging)");
    return texture;
}

[[nodiscard]] std::vector<ReadbackPixel> readback(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    ID3D11Texture2D* source)
{
    const ComPtr<ID3D11Texture2D> staging = createStagingTexture(device);
    context->CopyResource(staging.Get(), source);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    throwIfFailed(
        context->Map(staging.Get(), 0U, D3D11_MAP_READ, 0U, &mapped),
        "ID3D11DeviceContext::Map(WARP staging)");
    std::vector<ReadbackPixel> pixels(
        static_cast<std::size_t>(testSize.width) * testSize.height);
    for (std::uint32_t y = 0U; y < testSize.height; ++y)
    {
        const auto* row = reinterpret_cast<const std::uint16_t*>(
            static_cast<const std::uint8_t*>(mapped.pData)
            + static_cast<std::size_t>(y) * mapped.RowPitch);
        for (std::uint32_t x = 0U; x < testSize.width; ++x)
        {
            const std::size_t sourceIndex = static_cast<std::size_t>(x) * 4U;
            pixels[static_cast<std::size_t>(y) * testSize.width + x] =
                ReadbackPixel{
                    halfToFloat(row[sourceIndex]),
                    halfToFloat(row[sourceIndex + 1U]),
                    halfToFloat(row[sourceIndex + 2U]),
                    halfToFloat(row[sourceIndex + 3U])};
        }
    }
    context->Unmap(staging.Get(), 0U);
    return pixels;
}

[[nodiscard]] bafx::fx::FrameSnapshot makeDiskSnapshot(const bool bloomEnabled)
{
    bafx::fx::FrameSnapshot snapshot{};
    snapshot.active = true;
    snapshot.sprites.push_back(bafx::fx::Sprite{
        bafx::fx::SpriteKind::CenterDisk,
        bafx::fx::PointF{
            static_cast<float>(testSize.width) * 0.5F,
            static_cast<float>(testSize.height) * 0.5F},
        48.0F,
        0.0F,
        bafx::fx::ColorF{1.0F, 1.0F, 1.0F, 1.0F},
        2.0F,
        0.0F,
        0U,
        4499,
        bloomEnabled});
    return snapshot;
}

[[nodiscard]] bafx::fx::FrameSnapshot makeTwoTrailSnapshot()
{
    bafx::fx::FrameSnapshot snapshot{};
    snapshot.active = true;
    snapshot.trailStrokes = {
        bafx::fx::TrailStroke{
            {
                bafx::fx::TrailPoint{bafx::fx::PointF{24.0F, 64.0F}, 0.0F},
                bafx::fx::TrailPoint{bafx::fx::PointF{112.0F, 64.0F}, 0.0F},
            },
            8.0F},
        bafx::fx::TrailStroke{
            {
                bafx::fx::TrailPoint{bafx::fx::PointF{144.0F, 192.0F}, 0.0F},
                bafx::fx::TrailPoint{bafx::fx::PointF{232.0F, 192.0F}, 0.0F},
            },
            8.0F}};
    return snapshot;
}

[[nodiscard]] float maximumRgbOutsideSprite(
    const std::vector<ReadbackPixel>& pixels) noexcept
{
    constexpr std::uint32_t margin = 28U;
    const std::uint32_t centerX = testSize.width / 2U;
    const std::uint32_t centerY = testSize.height / 2U;
    float maximum = 0.0F;
    for (std::uint32_t y = 0U; y < testSize.height; ++y)
    {
        for (std::uint32_t x = 0U; x < testSize.width; ++x)
        {
            if (x + margin >= centerX
                && x <= centerX + margin
                && y + margin >= centerY
                && y <= centerY + margin)
            {
                continue;
            }
            const ReadbackPixel& pixel =
                pixels[static_cast<std::size_t>(y) * testSize.width + x];
            maximum = std::max(
                maximum,
                std::max({pixel.red, pixel.green, pixel.blue}));
        }
    }
    return maximum;
}

[[nodiscard]] float maximumRgbInBox(
    const std::vector<ReadbackPixel>& pixels,
    const std::uint32_t left,
    const std::uint32_t top,
    const std::uint32_t right,
    const std::uint32_t bottom) noexcept
{
    float maximum = 0.0F;
    for (std::uint32_t y = top; y < bottom; ++y)
    {
        for (std::uint32_t x = left; x < right; ++x)
        {
            const ReadbackPixel& pixel =
                pixels[static_cast<std::size_t>(y) * testSize.width + x];
            maximum = std::max(
                maximum,
                std::max({pixel.red, pixel.green, pixel.blue}));
        }
    }
    return maximum;
}

void checkFiniteAndNonNegative(const std::vector<ReadbackPixel>& pixels)
{
    for (const ReadbackPixel& pixel : pixels)
    {
        BAFX_CHECK(std::isfinite(pixel.red));
        BAFX_CHECK(std::isfinite(pixel.green));
        BAFX_CHECK(std::isfinite(pixel.blue));
        BAFX_CHECK(std::isfinite(pixel.alpha));
        BAFX_CHECK(pixel.red >= negativeTolerance);
        BAFX_CHECK(pixel.green >= negativeTolerance);
        BAFX_CHECK(pixel.blue >= negativeTolerance);
        BAFX_CHECK(pixel.alpha >= negativeTolerance);
    }
}

}

BAFX_TEST(warp_pipeline_separates_direct_emission_and_multilevel_bloom_seed)
{
    ComApartment apartment;
    const WarpDevice graphics = createWarpDevice();
    BAFX_CHECK(graphics.featureLevel >= D3D_FEATURE_LEVEL_11_0);

    FxGpuRenderer renderer(graphics.device.Get(), graphics.context.Get(), testSize);
    const RenderTarget withoutBloom = createRenderTarget(graphics.device.Get());
    renderer.render(makeDiskSnapshot(false), withoutBloom.view.Get());
    const std::vector<ReadbackPixel> directPixels = readback(
        graphics.device.Get(),
        graphics.context.Get(),
        withoutBloom.texture.Get());

    const RenderTarget withBloom = createRenderTarget(graphics.device.Get());
    renderer.render(makeDiskSnapshot(true), withBloom.view.Get());
    const std::vector<ReadbackPixel> bloomPixels = readback(
        graphics.device.Get(),
        graphics.context.Get(),
        withBloom.texture.Get());

    checkFiniteAndNonNegative(directPixels);
    checkFiniteAndNonNegative(bloomPixels);
    const std::size_t center = static_cast<std::size_t>(testSize.height / 2U)
        * testSize.width
        + testSize.width / 2U;
    BAFX_CHECK(directPixels[center].blue > 1.0F);
    BAFX_CHECK(directPixels[center].alpha > 0.5F);
    BAFX_CHECK_NEAR(
        bloomPixels[center].alpha,
        directPixels[center].alpha,
        1.0e-3F);
    BAFX_CHECK(maximumRgbOutsideSprite(directPixels) <= 1.0e-6F);
    BAFX_CHECK(maximumRgbOutsideSprite(bloomPixels) > 1.0e-3F);
}

BAFX_TEST(warp_pipeline_renders_every_retained_trail_stroke)
{
    ComApartment apartment;
    const WarpDevice graphics = createWarpDevice();
    FxGpuRenderer renderer(graphics.device.Get(), graphics.context.Get(), testSize);
    const RenderTarget target = createRenderTarget(graphics.device.Get());

    renderer.render(makeTwoTrailSnapshot(), target.view.Get());
    const std::vector<ReadbackPixel> pixels = readback(
        graphics.device.Get(),
        graphics.context.Get(),
        target.texture.Get());

    checkFiniteAndNonNegative(pixels);
    BAFX_CHECK(maximumRgbInBox(pixels, 16U, 48U, 120U, 80U) > 1.0e-3F);
    BAFX_CHECK(maximumRgbInBox(pixels, 136U, 176U, 240U, 208U) > 1.0e-3F);
}
