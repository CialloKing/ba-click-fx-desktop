#include "test_support.hpp"

#include "bafx/windows/error.hpp"
#include "bafx/windows/fx_gpu_renderer.hpp"
#include "bafx/windows/gpu_texture_readback.hpp"

#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
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
    ComPtr<ID3D11ShaderResourceView> shaderResource{};
};

struct WarpDevice
{
    ComPtr<ID3D11Device> device{};
    ComPtr<ID3D11DeviceContext> context{};
    D3D_FEATURE_LEVEL featureLevel{D3D_FEATURE_LEVEL_11_0};
};

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
    description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    RenderTarget target{};
    throwIfFailed(
        device->CreateTexture2D(&description, nullptr, &target.texture),
        "ID3D11Device::CreateTexture2D(WARP destination)");
    throwIfFailed(
        device->CreateRenderTargetView(target.texture.Get(), nullptr, &target.view),
        "ID3D11Device::CreateRenderTargetView(WARP destination)");
    throwIfFailed(
        device->CreateShaderResourceView(
            target.texture.Get(),
            nullptr,
            &target.shaderResource),
        "ID3D11Device::CreateShaderResourceView(WARP destination)");
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

[[nodiscard]] std::vector<ReadbackPixel> readback(
    ID3D11DeviceContext* context,
    ID3D11Texture2D* source)
{
    const Rgba16FloatImage raw = readbackRgba16FloatTexture(context, source);
    std::vector<ReadbackPixel> pixels;
    pixels.reserve(raw.pixels.size());
    for (const Rgba16FloatPixel pixel : raw.pixels)
    {
        pixels.push_back(ReadbackPixel{
            halfToFloat(pixel.red),
            halfToFloat(pixel.green),
            halfToFloat(pixel.blue),
            halfToFloat(pixel.alpha)});
    }
    return pixels;
}

[[nodiscard]] std::vector<ReadbackPixel> toFloatPixels(
    const Rgba16FloatImage& raw)
{
    std::vector<ReadbackPixel> pixels;
    pixels.reserve(raw.pixels.size());
    for (const Rgba16FloatPixel pixel : raw.pixels)
    {
        pixels.push_back(ReadbackPixel{
            halfToFloat(pixel.red),
            halfToFloat(pixel.green),
            halfToFloat(pixel.blue),
            halfToFloat(pixel.alpha)});
    }
    return pixels;
}

[[nodiscard]] bool isZeroImage(const Rgba16FloatImage& image) noexcept
{
    return std::all_of(
        image.pixels.begin(),
        image.pixels.end(),
        [](const Rgba16FloatPixel pixel)
        {
            return pixel.red == 0U
                && pixel.green == 0U
                && pixel.blue == 0U
                && pixel.alpha == 0U;
        });
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

[[nodiscard]] bafx::fx::FrameSnapshot makeTriangleSnapshot()
{
    bafx::fx::FrameSnapshot snapshot{};
    snapshot.active = true;
    snapshot.sprites.push_back(bafx::fx::Sprite{
        bafx::fx::SpriteKind::Triangle,
        bafx::fx::PointF{
            static_cast<float>(testSize.width) * 0.5F,
            static_cast<float>(testSize.height) * 0.5F},
        48.0F,
        0.0F,
        bafx::fx::ColorF{1.0F, 1.0F, 1.0F, 1.0F},
        5.992157F,
        0.0F,
        0U,
        4550,
        false});
    return snapshot;
}

[[nodiscard]] bafx::fx::FrameSnapshot makeSeparatedDiskAndTriangleSnapshot(
    const float triangleAlpha)
{
    bafx::fx::FrameSnapshot snapshot = makeDiskSnapshot(false);
    snapshot.sprites.front().centerPixels.y =
        static_cast<float>(testSize.height) * 0.6875F;
    snapshot.sprites.push_back(bafx::fx::Sprite{
        bafx::fx::SpriteKind::Triangle,
        bafx::fx::PointF{
            static_cast<float>(testSize.width) * 0.5F,
            static_cast<float>(testSize.height) * 0.25F},
        48.0F,
        0.0F,
        bafx::fx::ColorF{1.0F, 1.0F, 1.0F, triangleAlpha},
        5.992157F,
        0.0F,
        0U,
        4550,
        false});
    return snapshot;
}

[[nodiscard]] bafx::fx::FrameSnapshot makeCoincidentDiskAndTriangleSnapshot(
    const float triangleAlpha,
    const bool includeDisk)
{
    bafx::fx::FrameSnapshot snapshot = includeDisk
        ? makeDiskSnapshot(false)
        : bafx::fx::FrameSnapshot{};
    snapshot.active = true;
    if (includeDisk)
    {
        // Cross2 keeps emitting while its lifecycle Coverage fades.
        snapshot.sprites.front().color.a = 0.05F;
    }
    snapshot.sprites.push_back(bafx::fx::Sprite{
        bafx::fx::SpriteKind::Triangle,
        bafx::fx::PointF{
            static_cast<float>(testSize.width) * 0.5F,
            static_cast<float>(testSize.height) * 0.5F},
        96.0F,
        0.0F,
        bafx::fx::ColorF{1.0F, 1.0F, 1.0F, triangleAlpha},
        5.992157F,
        0.0F,
        0U,
        4550,
        false});
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

[[nodiscard]] float maximumAlphaInBox(
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
            maximum = std::max(
                maximum,
                pixels[static_cast<std::size_t>(y) * testSize.width + x].alpha);
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

void checkValidDesktopPremultiplied(
    const std::vector<ReadbackPixel>& pixels,
    const bool enforceFxOnlyAlphaLimit = true)
{
    constexpr float overlayAlphaLimit = 250.0F / 255.0F;
    constexpr float transportTolerance = 2.0e-3F;
    checkFiniteAndNonNegative(pixels);
    for (const ReadbackPixel& pixel : pixels)
    {
        if (enforceFxOnlyAlphaLimit)
        {
            BAFX_CHECK(pixel.alpha <= overlayAlphaLimit + transportTolerance);
        }
        else
        {
            BAFX_CHECK(pixel.alpha <= 1.0F + transportTolerance);
        }
        BAFX_CHECK(pixel.red <= pixel.alpha + transportTolerance);
        BAFX_CHECK(pixel.green <= pixel.alpha + transportTolerance);
        BAFX_CHECK(pixel.blue <= pixel.alpha + transportTolerance);
    }
}

[[nodiscard]] float maximumRgbaDelta(
    const std::vector<ReadbackPixel>& left,
    const std::vector<ReadbackPixel>& right) noexcept
{
    float maximum = 0.0F;
    for (std::size_t index = 0U; index < left.size(); ++index)
    {
        maximum = std::max(
            maximum,
            std::max({
                std::abs(left[index].red - right[index].red),
                std::abs(left[index].green - right[index].green),
                std::abs(left[index].blue - right[index].blue),
                std::abs(left[index].alpha - right[index].alpha)}));
    }
    return maximum;
}

[[nodiscard]] ReadbackPixel compositeOver(
    const ReadbackPixel foreground,
    const std::array<float, 4>& background) noexcept
{
    const float backgroundWeight = 1.0F - foreground.alpha;
    return ReadbackPixel{
        foreground.red + background[0] * backgroundWeight,
        foreground.green + background[1] * backgroundWeight,
        foreground.blue + background[2] * backgroundWeight,
        1.0F};
}

[[nodiscard]] float maximumCompositeRgbDeltaInBox(
    const std::vector<ReadbackPixel>& left,
    const std::vector<ReadbackPixel>& right,
    const std::array<float, 4>& background,
    const std::uint32_t boxLeft,
    const std::uint32_t boxTop,
    const std::uint32_t boxRight,
    const std::uint32_t boxBottom) noexcept
{
    float maximum = 0.0F;
    for (std::uint32_t y = boxTop; y < boxBottom; ++y)
    {
        for (std::uint32_t x = boxLeft; x < boxRight; ++x)
        {
            const std::size_t index = static_cast<std::size_t>(y)
                * testSize.width
                + x;
            const ReadbackPixel leftComposite = compositeOver(left[index], background);
            const ReadbackPixel rightComposite = compositeOver(right[index], background);
            maximum = std::max(
                maximum,
                std::max({
                    std::abs(leftComposite.red - rightComposite.red),
                    std::abs(leftComposite.green - rightComposite.green),
                    std::abs(leftComposite.blue - rightComposite.blue)}));
        }
    }
    return maximum;
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
        graphics.context.Get(),
        withoutBloom.texture.Get());

    const RenderTarget withBloom = createRenderTarget(graphics.device.Get());
    renderer.render(makeDiskSnapshot(true), withBloom.view.Get());
    const std::vector<ReadbackPixel> bloomPixels = readback(
        graphics.context.Get(),
        withBloom.texture.Get());

    checkValidDesktopPremultiplied(directPixels);
    checkValidDesktopPremultiplied(bloomPixels);
    const std::size_t center = static_cast<std::size_t>(testSize.height / 2U)
        * testSize.width
        + testSize.width / 2U;
    BAFX_CHECK(directPixels[center].blue > 0.5F);
    BAFX_CHECK(directPixels[center].alpha > 0.5F);
    BAFX_CHECK_NEAR(
        bloomPixels[center].alpha,
        directPixels[center].alpha,
        1.0e-3F);
    BAFX_CHECK(maximumRgbOutsideSprite(directPixels) <= 1.0e-6F);
    BAFX_CHECK(maximumRgbOutsideSprite(bloomPixels) > 1.0e-3F);
}

BAFX_TEST(warp_background_bloom_fades_with_valid_desktop_transport)
{
    ComApartment apartment;
    const WarpDevice graphics = createWarpDevice();
    FxGpuRenderer renderer(graphics.device.Get(), graphics.context.Get(), testSize);
    const bafx::fx::FrameSnapshot snapshot = makeDiskSnapshot(true);

    const RenderTarget background = createRenderTarget(graphics.device.Get());
    constexpr std::array<float, 4> backgroundColor{0.75F, 0.25F, 0.5F, 1.0F};
    graphics.context->ClearRenderTargetView(
        background.view.Get(),
        backgroundColor.data());

    const RenderTarget fxOnlyTarget = createRenderTarget(graphics.device.Get());
    renderer.render(snapshot, fxOnlyTarget.view.Get());
    const std::vector<ReadbackPixel> fxOnly = readback(
        graphics.context.Get(),
        fxOnlyTarget.texture.Get());

    const auto renderWithWeight = [&](const float weight)
    {
        const RenderTarget target = createRenderTarget(graphics.device.Get());
        renderer.render(
            snapshot,
            target.view.Get(),
            BackgroundRenderInput{background.shaderResource.Get(), weight});
        return readback(graphics.context.Get(), target.texture.Get());
    };
    const std::vector<ReadbackPixel> zeroWeight = renderWithWeight(0.0F);
    const std::vector<ReadbackPixel> halfWeight = renderWithWeight(0.5F);
    const std::vector<ReadbackPixel> fullWeight = renderWithWeight(1.0F);

    checkValidDesktopPremultiplied(fxOnly);
    checkValidDesktopPremultiplied(zeroWeight, false);
    checkValidDesktopPremultiplied(halfWeight, false);
    checkValidDesktopPremultiplied(fullWeight, false);

    // A zero Differential Bloom weight still has a valid WGC sample, so the
    // transparent transport must remain background-aware until the caller
    // removes that sample explicitly.
    BAFX_CHECK(maximumRgbaDelta(fxOnly, zeroWeight) > 1.0e-3F);
    const float fullFadeDelta = maximumRgbaDelta(zeroWeight, fullWeight);
    const float halfFromZero = maximumRgbaDelta(zeroWeight, halfWeight);
    const float halfToFull = maximumRgbaDelta(halfWeight, fullWeight);
    BAFX_CHECK(fullFadeDelta > 1.0e-3F);
    BAFX_CHECK(halfFromZero > 1.0e-3F);
    BAFX_CHECK(halfFromZero < fullFadeDelta);
    BAFX_CHECK(halfToFull < fullFadeDelta);

    const std::size_t center = static_cast<std::size_t>(testSize.height / 2U)
        * testSize.width
        + testSize.width / 2U;
    BAFX_CHECK(zeroWeight[center].blue > 0.5F);
    BAFX_CHECK(fullWeight[center].blue > 0.5F);
}

BAFX_TEST(warp_background_reconstructs_the_unity_source_over_target)
{
    ComApartment apartment;
    const WarpDevice graphics = createWarpDevice();
    FxGpuRenderer renderer(graphics.device.Get(), graphics.context.Get(), testSize);
    renderer.setBloomSettings(FxBloomSettings{0.0F, 7.0F});
    const bafx::fx::FrameSnapshot snapshot = makeDiskSnapshot(false);
    const RenderTarget captureTarget = createRenderTarget(graphics.device.Get());
    const FxGpuFrameCapture capture = renderer.renderAndCapture(
        snapshot,
        captureTarget.view.Get());
    const std::vector<ReadbackPixel> direct = toFloatPixels(capture.directSurface);

    std::size_t edgeIndex = direct.size();
    for (std::size_t index = 0U; index < direct.size(); ++index)
    {
        if (direct[index].alpha > 0.15F && direct[index].alpha < 0.75F)
        {
            edgeIndex = index;
            break;
        }
    }
    BAFX_CHECK(edgeIndex < direct.size());

    const auto renderOver = [&](const std::array<float, 4>& color)
    {
        const RenderTarget background = createRenderTarget(graphics.device.Get());
        graphics.context->ClearRenderTargetView(background.view.Get(), color.data());
        const RenderTarget output = createRenderTarget(graphics.device.Get());
        renderer.render(
            snapshot,
            output.view.Get(),
            BackgroundRenderInput{background.shaderResource.Get(), 1.0F});
        return readback(graphics.context.Get(), output.texture.Get());
    };

    constexpr std::array<float, 4> darkBackground{0.1F, 0.2F, 0.3F, 1.0F};
    constexpr std::array<float, 4> lightBackground{0.7F, 0.6F, 0.5F, 1.0F};
    const std::vector<ReadbackPixel> overDark = renderOver(darkBackground);
    const std::vector<ReadbackPixel> overLight = renderOver(lightBackground);
    checkValidDesktopPremultiplied(overDark, false);
    checkValidDesktopPremultiplied(overLight, false);

    const ReadbackPixel darkReconstructed = compositeOver(
        overDark[edgeIndex],
        darkBackground);
    const ReadbackPixel lightReconstructed = compositeOver(
        overLight[edgeIndex],
        lightBackground);
    const float coverage = direct[edgeIndex].alpha;
    const ReadbackPixel expectedDark{
        std::clamp(direct[edgeIndex].red
            + darkBackground[0] * (1.0F - coverage), 0.0F, 1.0F),
        std::clamp(direct[edgeIndex].green
            + darkBackground[1] * (1.0F - coverage), 0.0F, 1.0F),
        std::clamp(direct[edgeIndex].blue
            + darkBackground[2] * (1.0F - coverage), 0.0F, 1.0F),
        1.0F};
    const ReadbackPixel expectedLight{
        std::clamp(direct[edgeIndex].red
            + lightBackground[0] * (1.0F - coverage), 0.0F, 1.0F),
        std::clamp(direct[edgeIndex].green
            + lightBackground[1] * (1.0F - coverage), 0.0F, 1.0F),
        std::clamp(direct[edgeIndex].blue
            + lightBackground[2] * (1.0F - coverage), 0.0F, 1.0F),
        1.0F};

    BAFX_CHECK_NEAR(darkReconstructed.red, expectedDark.red, 4.0e-3F);
    BAFX_CHECK_NEAR(darkReconstructed.green, expectedDark.green, 4.0e-3F);
    BAFX_CHECK_NEAR(darkReconstructed.blue, expectedDark.blue, 4.0e-3F);
    BAFX_CHECK_NEAR(lightReconstructed.red, expectedLight.red, 4.0e-3F);
    BAFX_CHECK_NEAR(lightReconstructed.green, expectedLight.green, 4.0e-3F);
    BAFX_CHECK_NEAR(lightReconstructed.blue, expectedLight.blue, 4.0e-3F);
    BAFX_CHECK(std::abs(lightReconstructed.red - darkReconstructed.red) > 0.05F);
}

BAFX_TEST(warp_wgc_weight_never_transfers_triangle_flicker_to_the_disk)
{
    ComApartment apartment;
    const WarpDevice graphics = createWarpDevice();
    FxGpuRenderer renderer(graphics.device.Get(), graphics.context.Get(), testSize);
    renderer.setBloomSettings(FxBloomSettings{0.0F, 7.0F});

    const RenderTarget background = createRenderTarget(graphics.device.Get());
    constexpr std::array<float, 4> backgroundColor{0.18F, 0.62F, 0.84F, 1.0F};
    graphics.context->ClearRenderTargetView(
        background.view.Get(),
        backgroundColor.data());

    const auto render = [&](const float triangleAlpha, const float bloomWeight)
    {
        const RenderTarget target = createRenderTarget(graphics.device.Get());
        renderer.render(
            makeSeparatedDiskAndTriangleSnapshot(triangleAlpha),
            target.view.Get(),
            BackgroundRenderInput{background.shaderResource.Get(), bloomWeight});
        return readback(graphics.context.Get(), target.texture.Get());
    };

    const std::vector<ReadbackPixel> brightTriangleZeroWeight = render(0.9F, 0.0F);
    const std::vector<ReadbackPixel> brightTriangleHalfWeight = render(0.9F, 0.5F);
    const std::vector<ReadbackPixel> brightTriangleFullWeight = render(0.9F, 1.0F);
    const std::vector<ReadbackPixel> dimTriangleHalfWeight = render(0.1F, 0.5F);

    checkValidDesktopPremultiplied(brightTriangleZeroWeight, false);
    checkValidDesktopPremultiplied(brightTriangleHalfWeight, false);
    checkValidDesktopPremultiplied(brightTriangleFullWeight, false);
    checkValidDesktopPremultiplied(dimTriangleHalfWeight, false);

    constexpr std::uint32_t diskLeft = 96U;
    constexpr std::uint32_t diskTop = 144U;
    constexpr std::uint32_t diskRight = 160U;
    constexpr std::uint32_t diskBottom = 208U;
    constexpr std::uint32_t triangleLeft = 96U;
    constexpr std::uint32_t triangleTop = 32U;
    constexpr std::uint32_t triangleRight = 160U;
    constexpr std::uint32_t triangleBottom = 96U;

    // Unity Cross2 and Tri2 own independent particle Alpha. WGC cadence may
    // fade Differential Bloom, but it must not make Cross2 inherit Tri2's pulse.
    BAFX_CHECK(maximumCompositeRgbDeltaInBox(
        brightTriangleZeroWeight,
        brightTriangleHalfWeight,
        backgroundColor,
        diskLeft,
        diskTop,
        diskRight,
        diskBottom) <= 1.0e-3F);
    BAFX_CHECK(maximumCompositeRgbDeltaInBox(
        brightTriangleHalfWeight,
        brightTriangleFullWeight,
        backgroundColor,
        diskLeft,
        diskTop,
        diskRight,
        diskBottom) <= 1.0e-3F);
    BAFX_CHECK(maximumCompositeRgbDeltaInBox(
        brightTriangleHalfWeight,
        dimTriangleHalfWeight,
        backgroundColor,
        diskLeft,
        diskTop,
        diskRight,
        diskBottom) <= 1.0e-3F);
    BAFX_CHECK(maximumCompositeRgbDeltaInBox(
        brightTriangleHalfWeight,
        dimTriangleHalfWeight,
        backgroundColor,
        triangleLeft,
        triangleTop,
        triangleRight,
        triangleBottom) > 0.05F);
}

BAFX_TEST(warp_fx_only_keeps_coincident_disk_independent_from_triangle_alpha)
{
    ComApartment apartment;
    const WarpDevice graphics = createWarpDevice();
    FxGpuRenderer renderer(graphics.device.Get(), graphics.context.Get(), testSize);
    renderer.setBloomSettings(FxBloomSettings{0.0F, 7.0F});

    const auto captureDirect = [&](const bafx::fx::FrameSnapshot& snapshot)
    {
        const RenderTarget target = createRenderTarget(graphics.device.Get());
        return toFloatPixels(
            renderer.renderAndCapture(snapshot, target.view.Get()).directSurface);
    };
    bafx::fx::FrameSnapshot fadedDisk = makeDiskSnapshot(false);
    fadedDisk.sprites.front().color.a = 0.05F;
    const std::vector<ReadbackPixel> diskDirect = captureDirect(fadedDisk);
    const std::vector<ReadbackPixel> brightTriangleDirect = captureDirect(
        makeCoincidentDiskAndTriangleSnapshot(0.9F, false));

    std::size_t overlapIndex = diskDirect.size();
    for (std::size_t index = 0U; index < diskDirect.size(); ++index)
    {
        const float diskCoverage = diskDirect[index].alpha;
        const float triangleCoverage = brightTriangleDirect[index].alpha;
        const float triangleEnergy = std::max({
            brightTriangleDirect[index].red,
            brightTriangleDirect[index].green,
            brightTriangleDirect[index].blue});
        if (diskCoverage >= 0.04F
            && diskCoverage <= 0.051F
            && triangleCoverage >= 0.8F
            && diskCoverage + triangleCoverage <= 0.97F
            && triangleEnergy >= 0.1F)
        {
            overlapIndex = index;
            break;
        }
    }
    BAFX_CHECK(overlapIndex < diskDirect.size());

    const auto render = [&](const float triangleAlpha, const bool includeDisk)
    {
        const RenderTarget target = createRenderTarget(graphics.device.Get());
        renderer.render(
            makeCoincidentDiskAndTriangleSnapshot(triangleAlpha, includeDisk),
            target.view.Get());
        return readback(graphics.context.Get(), target.texture.Get());
    };
    const std::vector<ReadbackPixel> combinedDim = render(0.1F, true);
    const std::vector<ReadbackPixel> combinedBright = render(0.9F, true);
    const std::vector<ReadbackPixel> triangleDim = render(0.1F, false);
    const std::vector<ReadbackPixel> triangleBright = render(0.9F, false);

    checkValidDesktopPremultiplied(combinedDim);
    checkValidDesktopPremultiplied(combinedBright);
    checkValidDesktopPremultiplied(triangleDim);
    checkValidDesktopPremultiplied(triangleBright);

    const ReadbackPixel dim = combinedDim[overlapIndex];
    const ReadbackPixel bright = combinedBright[overlapIndex];
    const ReadbackPixel dimTriangle = triangleDim[overlapIndex];
    const ReadbackPixel brightTriangle = triangleBright[overlapIndex];
    const std::array<float, 3> diskWithDimTriangle{
        dim.red - dimTriangle.red,
        dim.green - dimTriangle.green,
        dim.blue - dimTriangle.blue};
    const std::array<float, 3> diskWithBrightTriangle{
        bright.red - brightTriangle.red,
        bright.green - brightTriangle.green,
        bright.blue - brightTriangle.blue};

    // On a black desktop the premultiplied RGB is the visible composite. Tri2
    // may pulse, but removing its own result must leave the same Cross2 disk.
    for (std::size_t channel = 0U; channel < diskWithDimTriangle.size(); ++channel)
    {
        BAFX_CHECK_NEAR(
            diskWithDimTriangle[channel],
            diskWithBrightTriangle[channel],
            3.0e-3F);
    }
    BAFX_CHECK(diskWithDimTriangle[2] > 0.01F);
    BAFX_CHECK(brightTriangle.blue - dimTriangle.blue > 0.05F);
    BAFX_CHECK(bright.alpha - dim.alpha > 0.1F);
}

BAFX_TEST(warp_pipeline_renders_every_retained_trail_stroke)
{
    ComApartment apartment;
    const WarpDevice graphics = createWarpDevice();
    FxGpuRenderer renderer(graphics.device.Get(), graphics.context.Get(), testSize);
    const RenderTarget target = createRenderTarget(graphics.device.Get());

    renderer.render(makeTwoTrailSnapshot(), target.view.Get());
    const std::vector<ReadbackPixel> pixels = readback(
        graphics.context.Get(),
        target.texture.Get());

    checkFiniteAndNonNegative(pixels);
    BAFX_CHECK(maximumRgbInBox(pixels, 16U, 48U, 120U, 80U) > 1.0e-3F);
    BAFX_CHECK(maximumRgbInBox(pixels, 136U, 176U, 240U, 208U) > 1.0e-3F);
}

BAFX_TEST(warp_trail_keeps_emission_and_desktop_coverage_independent)
{
    ComApartment apartment;
    const WarpDevice graphics = createWarpDevice();
    FxGpuRenderer renderer(graphics.device.Get(), graphics.context.Get(), testSize);
    renderer.setBloomSettings(FxBloomSettings{0.0F, 7.0F});
    const RenderTarget target = createRenderTarget(graphics.device.Get());
    const bafx::fx::FrameSnapshot snapshot = makeTwoTrailSnapshot();

    const FxGpuFrameCapture capture = renderer.renderAndCapture(
        snapshot,
        target.view.Get());
    BAFX_CHECK(capture.intermediateLayersValid);
    const std::vector<ReadbackPixel> direct = toFloatPixels(capture.directSurface);

    renderer.render(snapshot, target.view.Get());
    const std::vector<ReadbackPixel> final = readback(
        graphics.context.Get(),
        target.texture.Get());
    checkValidDesktopPremultiplied(final);

    // The old endpoint remains visible in geometry, but its transport envelope is zero.
    BAFX_CHECK(maximumAlphaInBox(direct, 16U, 56U, 44U, 72U) < 1.0e-3F);
    BAFX_CHECK(maximumAlphaInBox(direct, 136U, 184U, 164U, 200U) < 1.0e-3F);
    BAFX_CHECK(maximumRgbInBox(direct, 88U, 56U, 120U, 72U) > 1.0e-3F);
    BAFX_CHECK(maximumRgbInBox(final, 16U, 56U, 44U, 72U) < 1.0e-3F);
    BAFX_CHECK(maximumRgbInBox(final, 136U, 184U, 164U, 200U) < 1.0e-3F);

    bool foundCoverageAboveVisibleEnergy = false;
    for (std::size_t index = 0U; index < direct.size(); ++index)
    {
        const float directEnergy = std::max({
            direct[index].red,
            direct[index].green,
            direct[index].blue});
        if (direct[index].alpha > 0.05F
            && direct[index].alpha > directEnergy + 0.01F)
        {
            foundCoverageAboveVisibleEnergy = true;
            BAFX_CHECK_NEAR(final[index].alpha, direct[index].alpha, 2.0e-3F);
            BAFX_CHECK_NEAR(final[index].red, direct[index].red, 1.0e-3F);
            BAFX_CHECK_NEAR(final[index].green, direct[index].green, 1.0e-3F);
            BAFX_CHECK_NEAR(final[index].blue, direct[index].blue, 1.0e-3F);
        }
    }
    BAFX_CHECK(foundCoverageAboveVisibleEnergy);
}

BAFX_TEST(warp_capture_reads_all_layers_from_the_same_frame)
{
    ComApartment apartment;
    const WarpDevice graphics = createWarpDevice();
    FxGpuRenderer renderer(graphics.device.Get(), graphics.context.Get(), testSize);
    const RenderTarget target = createRenderTarget(graphics.device.Get());

    const FxGpuFrameCapture capture = renderer.renderAndCapture(
        makeDiskSnapshot(true),
        target.view.Get());

    BAFX_CHECK(capture.intermediateLayersValid);
    BAFX_CHECK(capture.directSurface.width == testSize.width);
    BAFX_CHECK(capture.directSurface.height == testSize.height);
    BAFX_CHECK(capture.bloomSeed.width == testSize.width);
    BAFX_CHECK(capture.finalOverlay.width == testSize.width);
    BAFX_CHECK(capture.bloomDown.size() == 4U);
    BAFX_CHECK(capture.bloomUp.size() == 3U);
    constexpr std::array expectedMipWidths{128U, 64U, 32U, 16U};
    for (std::size_t index = 0U; index < expectedMipWidths.size(); ++index)
    {
        BAFX_CHECK(capture.bloomDown[index].width == expectedMipWidths[index]);
        BAFX_CHECK(capture.bloomDown[index].height == expectedMipWidths[index]);
    }
    for (std::size_t index = 0U; index < capture.bloomUp.size(); ++index)
    {
        BAFX_CHECK(capture.bloomUp[index].width == expectedMipWidths[index]);
        BAFX_CHECK(capture.bloomUp[index].height == expectedMipWidths[index]);
    }

    const std::vector<ReadbackPixel> direct = toFloatPixels(capture.directSurface);
    const std::vector<ReadbackPixel> seed = toFloatPixels(capture.bloomSeed);
    const std::vector<ReadbackPixel> final = toFloatPixels(capture.finalOverlay);
    checkFiniteAndNonNegative(direct);
    checkFiniteAndNonNegative(seed);
    checkFiniteAndNonNegative(final);
    const std::size_t center = static_cast<std::size_t>(testSize.height / 2U)
        * testSize.width
        + testSize.width / 2U;
    BAFX_CHECK(direct[center].blue > 1.0F);
    BAFX_CHECK(seed[center].blue > 1.0F);
    BAFX_CHECK(final[center].blue >= direct[center].blue);
}

BAFX_TEST(warp_pipeline_applies_runtime_bloom_intensity_and_quality)
{
    ComApartment apartment;
    const WarpDevice graphics = createWarpDevice();
    FxGpuRenderer renderer(graphics.device.Get(), graphics.context.Get(), testSize);
    const RenderTarget target = createRenderTarget(graphics.device.Get());

    renderer.setBloomSettings(FxBloomSettings{0.0F, 7.0F});
    const FxGpuFrameCapture disabledBloom = renderer.renderAndCapture(
        makeDiskSnapshot(true),
        target.view.Get());
    const std::vector<ReadbackPixel> disabledPixels = toFloatPixels(
        disabledBloom.finalOverlay);
    BAFX_CHECK(disabledBloom.bloomDown.size() == 4U);
    BAFX_CHECK(maximumRgbOutsideSprite(disabledPixels) <= 1.0e-6F);

    renderer.setBloomSettings(FxBloomSettings{1.0F, 10.0F});
    const FxGpuFrameCapture ultraBloom = renderer.renderAndCapture(
        makeDiskSnapshot(true),
        target.view.Get());
    BAFX_CHECK(ultraBloom.bloomDown.size() == 7U);
    BAFX_CHECK(ultraBloom.bloomUp.size() == 6U);

    renderer.setBloomSettings(FxBloomSettings{1.0F, 4.0F});
    const FxGpuFrameCapture lowBloom = renderer.renderAndCapture(
        makeDiskSnapshot(true),
        target.view.Get());
    BAFX_CHECK(lowBloom.bloomDown.size() == 1U);
    BAFX_CHECK(lowBloom.bloomUp.empty());
}

BAFX_TEST(warp_capture_proves_triangle_bloom_seed_is_zero)
{
    ComApartment apartment;
    const WarpDevice graphics = createWarpDevice();
    FxGpuRenderer renderer(graphics.device.Get(), graphics.context.Get(), testSize);
    const RenderTarget target = createRenderTarget(graphics.device.Get());

    const FxGpuFrameCapture capture = renderer.renderAndCapture(
        makeTriangleSnapshot(),
        target.view.Get());

    BAFX_CHECK(capture.intermediateLayersValid);
    BAFX_CHECK(!isZeroImage(capture.directSurface));
    BAFX_CHECK(isZeroImage(capture.bloomSeed));
    for (const Rgba16FloatImage& mip : capture.bloomDown)
    {
        BAFX_CHECK(isZeroImage(mip));
    }
    for (const Rgba16FloatImage& mip : capture.bloomUp)
    {
        BAFX_CHECK(isZeroImage(mip));
    }
    BAFX_CHECK(!isZeroImage(capture.finalOverlay));
}

BAFX_TEST(warp_capture_uses_source_over_for_overlapping_cross_bloom_seed)
{
    ComApartment apartment;
    const WarpDevice graphics = createWarpDevice();
    FxGpuRenderer renderer(graphics.device.Get(), graphics.context.Get(), testSize);
    const RenderTarget singleTarget = createRenderTarget(graphics.device.Get());
    const FxGpuFrameCapture single = renderer.renderAndCapture(
        makeDiskSnapshot(true),
        singleTarget.view.Get());

    bafx::fx::FrameSnapshot overlapping = makeDiskSnapshot(true);
    overlapping.sprites.push_back(overlapping.sprites.front());
    const RenderTarget overlappingTarget = createRenderTarget(graphics.device.Get());
    const FxGpuFrameCapture doubled = renderer.renderAndCapture(
        overlapping,
        overlappingTarget.view.Get());

    const std::size_t center = static_cast<std::size_t>(testSize.height / 2U)
        * testSize.width
        + testSize.width / 2U;
    const ReadbackPixel singleSeed = toFloatPixels(single.bloomSeed)[center];
    const ReadbackPixel doubledSeed = toFloatPixels(doubled.bloomSeed)[center];
    BAFX_CHECK(singleSeed.alpha > 0.99F);
    BAFX_CHECK_NEAR(doubledSeed.red, singleSeed.red, 1.0e-3F);
    BAFX_CHECK_NEAR(doubledSeed.green, singleSeed.green, 1.0e-3F);
    BAFX_CHECK_NEAR(doubledSeed.blue, singleSeed.blue, 1.0e-3F);
    BAFX_CHECK_NEAR(doubledSeed.alpha, singleSeed.alpha, 1.0e-3F);
}

BAFX_TEST(warp_capture_never_exports_stale_layers_for_an_empty_frame)
{
    ComApartment apartment;
    const WarpDevice graphics = createWarpDevice();
    FxGpuRenderer renderer(graphics.device.Get(), graphics.context.Get(), testSize);
    const RenderTarget target = createRenderTarget(graphics.device.Get());
    renderer.render(makeDiskSnapshot(true), target.view.Get());

    const FxGpuFrameCapture capture = renderer.renderAndCapture(
        bafx::fx::FrameSnapshot{},
        target.view.Get());

    BAFX_CHECK(!capture.intermediateLayersValid);
    BAFX_CHECK(capture.directSurface.pixels.empty());
    BAFX_CHECK(capture.bloomSeed.pixels.empty());
    BAFX_CHECK(capture.bloomDown.empty());
    BAFX_CHECK(capture.bloomUp.empty());
    BAFX_CHECK(isZeroImage(capture.finalOverlay));
}
