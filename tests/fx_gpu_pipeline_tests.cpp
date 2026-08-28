#include "test_support.hpp"

#include "bafx/core/background_freshness.hpp"
#include "bafx/fx/frame_bounds.hpp"
#include "bafx/windows/detail/active_fx_roi_plan_validation_cache.hpp"
#include "bafx/windows/error.hpp"
#include "bafx/windows/fx_gpu_renderer.hpp"
#include "bafx/windows/gpu_texture_readback.hpp"

#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace bafx::windows;
using namespace std::chrono_literals;

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

struct Bgra8Image
{
    std::uint32_t width{0U};
    std::uint32_t height{0U};
    std::vector<Bgra8UnormPixel> pixels{};
};

struct WarpDevice
{
    ComPtr<ID3D11Device> device{};
    ComPtr<ID3D11DeviceContext> context{};
    D3D_FEATURE_LEVEL featureLevel{D3D_FEATURE_LEVEL_11_0};
};

[[nodiscard]] RenderTarget createRenderTarget(
    ID3D11Device* device,
    const WindowSize size = testSize)
{
    D3D11_TEXTURE2D_DESC description{};
    description.Width = size.width;
    description.Height = size.height;
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

[[nodiscard]] RenderTarget createRecordingRenderTarget(
    ID3D11Device* device,
    const WindowSize size = testSize)
{
    D3D11_TEXTURE2D_DESC description{};
    description.Width = size.width;
    description.Height = size.height;
    description.MipLevels = 1U;
    description.ArraySize = 1U;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc = DXGI_SAMPLE_DESC{1U, 0U};
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_RENDER_TARGET;

    RenderTarget target{};
    throwIfFailed(
        device->CreateTexture2D(&description, nullptr, &target.texture),
        "ID3D11Device::CreateTexture2D(Spout2 recording target)");
    throwIfFailed(
        device->CreateRenderTargetView(target.texture.Get(), nullptr, &target.view),
        "ID3D11Device::CreateRenderTargetView(Spout2 recording target)");
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

[[nodiscard]] Bgra8Image readbackBgra8(
    ID3D11DeviceContext* context,
    ID3D11Texture2D* source)
{
    D3D11_TEXTURE2D_DESC sourceDescription{};
    source->GetDesc(&sourceDescription);
    if (sourceDescription.Format != DXGI_FORMAT_B8G8R8A8_UNORM
        || sourceDescription.ArraySize != 1U
        || sourceDescription.MipLevels != 1U
        || sourceDescription.SampleDesc.Count != 1U)
    {
        throw std::invalid_argument("BGRA8 readback requires one single-sample texture");
    }

    D3D11_TEXTURE2D_DESC stagingDescription = sourceDescription;
    stagingDescription.Usage = D3D11_USAGE_STAGING;
    stagingDescription.BindFlags = 0U;
    stagingDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDescription.MiscFlags = 0U;

    ComPtr<ID3D11Device> device;
    source->GetDevice(&device);
    ComPtr<ID3D11Texture2D> staging;
    throwIfFailed(
        device->CreateTexture2D(&stagingDescription, nullptr, &staging),
        "ID3D11Device::CreateTexture2D(BGRA8 staging)");
    context->CopyResource(staging.Get(), source);

    Bgra8Image image{};
    image.width = sourceDescription.Width;
    image.height = sourceDescription.Height;
    image.pixels.resize(
        static_cast<std::size_t>(image.width) * image.height);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    throwIfFailed(
        context->Map(staging.Get(), 0U, D3D11_MAP_READ, 0U, &mapped),
        "ID3D11DeviceContext::Map(BGRA8 staging)");
    for (std::uint32_t y = 0U; y < image.height; ++y)
    {
        const auto* row = reinterpret_cast<const Bgra8UnormPixel*>(
            static_cast<const std::uint8_t*>(mapped.pData)
            + static_cast<std::size_t>(y) * mapped.RowPitch);
        std::copy_n(
            row,
            image.width,
            image.pixels.begin() + static_cast<std::size_t>(y) * image.width);
    }
    context->Unmap(staging.Get(), 0U);
    return image;
}

[[nodiscard]] bool hasExtendedEmission(const Bgra8Image& image) noexcept
{
    return std::any_of(
        image.pixels.begin(),
        image.pixels.end(),
        [](const Bgra8UnormPixel pixel)
        {
            return pixel.red > pixel.alpha
                || pixel.green > pixel.alpha
                || pixel.blue > pixel.alpha;
        });
}

[[nodiscard]] bool hasZeroAlphaEmission(const Bgra8Image& image) noexcept
{
    return std::any_of(
        image.pixels.begin(),
        image.pixels.end(),
        [](const Bgra8UnormPixel pixel)
        {
            return pixel.alpha == 0U
                && (pixel.red != 0U || pixel.green != 0U || pixel.blue != 0U);
        });
}

[[nodiscard]] bool isTransparent(const Bgra8Image& image) noexcept
{
    return std::all_of(
        image.pixels.begin(),
        image.pixels.end(),
        [](const Bgra8UnormPixel pixel)
        {
            return pixel.red == 0U
                && pixel.green == 0U
                && pixel.blue == 0U
                && pixel.alpha == 0U;
        });
}

[[nodiscard]] bool sameBgra8(
    const Bgra8Image& left,
    const Bgra8Image& right) noexcept
{
    if (left.width != right.width
        || left.height != right.height
        || left.pixels.size() != right.pixels.size())
    {
        return false;
    }
    return std::equal(
        left.pixels.begin(),
        left.pixels.end(),
        right.pixels.begin(),
        [](const Bgra8UnormPixel first, const Bgra8UnormPixel second)
        {
            return first.red == second.red
                && first.green == second.green
                && first.blue == second.blue
                && first.alpha == second.alpha;
        });
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

void checkRgba16BitExactAndFinite(
    const Rgba16FloatImage& expected,
    const Rgba16FloatImage& actual,
    const std::string_view layer = "rgba16")
{
    BAFX_CHECK(expected.width == actual.width);
    BAFX_CHECK(expected.height == actual.height);
    BAFX_CHECK(expected.pixels.size() == actual.pixels.size());
    for (std::size_t index = 0U; index < expected.pixels.size(); ++index)
    {
        const Rgba16FloatPixel reference = expected.pixels[index];
        const Rgba16FloatPixel candidate = actual.pixels[index];
        const auto requireChannel = [layer, index](
                                        const std::uint16_t referenceBits,
                                        const std::uint16_t candidateBits,
                                        const std::string_view channel)
        {
            if (referenceBits != candidateBits)
            {
                throw std::runtime_error(
                    std::string(layer) + " " + std::string(channel)
                    + " differs at pixel " + std::to_string(index));
            }
            if (!std::isfinite(halfToFloat(candidateBits)))
            {
                throw std::runtime_error(
                    std::string(layer) + " " + std::string(channel)
                    + " is not finite at pixel " + std::to_string(index));
            }
        };
        requireChannel(reference.red, candidate.red, "red");
        requireChannel(reference.green, candidate.green, "green");
        requireChannel(reference.blue, candidate.blue, "blue");
        requireChannel(reference.alpha, candidate.alpha, "alpha");
    }
}

void checkRgba16LayersBitExactAndFinite(
    const std::vector<Rgba16FloatImage>& expected,
    const std::vector<Rgba16FloatImage>& actual,
    const std::string_view layer)
{
    BAFX_CHECK(expected.size() == actual.size());
    for (std::size_t index = 0U; index < expected.size(); ++index)
    {
        checkRgba16BitExactAndFinite(
            expected[index],
            actual[index],
            std::string(layer) + " " + std::to_string(index));
    }
}

void checkCaptureBitExactAndFinite(
    const FxGpuFrameCapture& expected,
    const FxGpuFrameCapture& actual)
{
    BAFX_CHECK(
        expected.intermediateLayersValid == actual.intermediateLayersValid);
    checkRgba16BitExactAndFinite(
        expected.directSurface,
        actual.directSurface,
        "direct surface");
    checkRgba16BitExactAndFinite(
        expected.bloomSeed,
        actual.bloomSeed,
        "bloom seed");
    checkRgba16LayersBitExactAndFinite(
        expected.bloomDown,
        actual.bloomDown,
        "bloom down");
    checkRgba16LayersBitExactAndFinite(
        expected.bloomUp,
        actual.bloomUp,
        "bloom up");
    checkRgba16BitExactAndFinite(
        expected.bloomResult,
        actual.bloomResult,
        "bloom result");
    checkRgba16BitExactAndFinite(
        expected.finalOverlay,
        actual.finalOverlay,
        "final overlay");
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

[[nodiscard]] bafx::fx::FrameSnapshot makeDiskSnapshotForSize(
    const WindowSize size,
    const bool bloomEnabled)
{
    bafx::fx::FrameSnapshot snapshot = makeDiskSnapshot(bloomEnabled);
    snapshot.sprites.front().centerPixels = bafx::fx::PointF{
        static_cast<float>(size.width) * 0.5F,
        static_cast<float>(size.height) * 0.5F};
    snapshot.sprites.front().sizePixels = 32.0F;
    return snapshot;
}

[[nodiscard]] bafx::core::UnityBloomPlan makeUnityBloomPlanForTest(
    const FxBloomSettings settings = {},
    const WindowSize size = testSize)
{
    const bafx::core::UnityBloomPlanResult bloom = bafx::core::planUnityBloom(
        bafx::core::BloomExtent{
            static_cast<std::int32_t>(size.width),
            static_cast<std::int32_t>(size.height)},
        bafx::core::UnityBloomSettings{
            settings.diffusion,
            0.0F,
            settings.intensity});
    bafx::test::check(
        bloom.status == bafx::core::UnityBloomStatus::Ok,
        "Unity Bloom plan is valid");
    return bloom.plan;
}

[[nodiscard]] FxActiveRoi makeActiveFxRoi(
    const bafx::core::RectI sourceSupport,
    const FxBloomSettings settings = {},
    const WindowSize size = testSize)
{
    const bafx::core::UnityBloomPlan bloom = makeUnityBloomPlanForTest(
        settings,
        size);
    const bafx::core::UnityBloomPassRoiPlanResult roi =
        bafx::core::planUnityBloomPassRoi(
            sourceSupport,
            bafx::core::RectI{
                0,
                0,
                static_cast<std::int32_t>(size.width),
                static_cast<std::int32_t>(size.height)},
            bloom);
    bafx::test::check(
        roi.status == bafx::core::RoiStatus::Ok,
        "Unity Bloom pass ROI plan is valid");
    return FxActiveRoi{roi.plan};
}

[[nodiscard]] FxActiveRoi makeActiveFxRoi(
    const bafx::fx::FrameSnapshot& snapshot,
    const FxBloomSettings settings = {},
    const WindowSize size = testSize)
{
    const bafx::fx::FrameVisualBoundsResult bounds =
        bafx::fx::visualBounds(snapshot);
    bafx::test::check(
        bounds.status == bafx::fx::FrameBoundsStatus::Ok,
        "snapshot has valid visual bounds");
    return makeActiveFxRoi(bounds.bounds, settings, size);
}

void checkActiveFxRoiUnavailableFallback(
    const FxGpuRendererFeaturePolicy featurePolicy)
{
    ComApartment apartment;
    const WarpDevice graphics = createWarpDevice();
    const bafx::fx::FrameSnapshot snapshot = makeDiskSnapshot(true);
    constexpr FxBloomSettings fourTapBloom{1.0F, 4.0F};

    FxGpuRenderer referenceRenderer(
        graphics.device.Get(),
        graphics.context.Get(),
        testSize,
        fourTapBloom);
    const RenderTarget referenceTarget = createRenderTarget(
        graphics.device.Get());
    referenceRenderer.render(snapshot, referenceTarget.view.Get());

    FxGpuRenderer fallbackRenderer(
        graphics.device.Get(),
        graphics.context.Get(),
        testSize,
        fourTapBloom,
        compositionOutputPolicyFor(
            CompositionOutputPreference::PreferLinearScRgb).mapping,
        featurePolicy);
    const RenderTarget fallbackTarget = createRenderTarget(
        graphics.device.Get());
    const FxRenderCpuDiagnostics diagnostics = fallbackRenderer.render(
        snapshot,
        fallbackTarget.view.Get(),
        std::nullopt,
        nullptr,
        nullptr,
        makeActiveFxRoi(snapshot, fourTapBloom));

    BAFX_CHECK(!diagnostics.activeFxRoiApplied);
    BAFX_CHECK(diagnostics.primaryActiveFxRoi.requested);
    BAFX_CHECK(diagnostics.primaryActiveFxRoi.eligible);
    BAFX_CHECK(diagnostics.primaryActiveFxRoi.executed);
    BAFX_CHECK(!diagnostics.primaryActiveFxRoi.warmup);
    BAFX_CHECK(
        diagnostics.primaryActiveFxRoi.actualPath
        == FxActiveRoiActualPath::Unavailable);
    BAFX_CHECK(
        diagnostics.primaryActiveFxRoi.decisionReason
        == FxActiveRoiDecisionReason::Context1Unavailable);
    BAFX_CHECK(
        diagnostics.primaryActiveFxRoi.drawnPixels
        == diagnostics.primaryActiveFxRoi.fullPixels);

    checkRgba16BitExactAndFinite(
        readbackRgba16FloatTexture(
            graphics.context.Get(),
            referenceTarget.texture.Get()),
        readbackRgba16FloatTexture(
            graphics.context.Get(),
            fallbackTarget.texture.Get()));

    const FxGpuFrameCapture referenceCapture = referenceRenderer.renderAndCapture(
        snapshot,
        referenceTarget.view.Get());
    const FxGpuFrameCapture fallbackCapture = fallbackRenderer.renderAndCapture(
        snapshot,
        fallbackTarget.view.Get(),
        makeActiveFxRoi(snapshot, fourTapBloom));
    checkCaptureBitExactAndFinite(referenceCapture, fallbackCapture);
}

[[nodiscard]] bafx::fx::FrameSnapshot makeDiskTransportSnapshot(
    const float particleAlpha,
    const float artisticIntensity)
{
    bafx::fx::FrameSnapshot snapshot = makeDiskSnapshot(false);
    snapshot.sprites.front().color.a = particleAlpha;
    snapshot.sprites.front().artisticIntensity = artisticIntensity;
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

[[nodiscard]] bafx::fx::FrameSnapshot makeCompactTrailSnapshot()
{
    bafx::fx::FrameSnapshot snapshot{};
    snapshot.active = true;
    snapshot.trailStrokes = {
        bafx::fx::TrailStroke{
            {
                bafx::fx::TrailPoint{bafx::fx::PointF{88.25F, 112.5F}, 0.0F},
                bafx::fx::TrailPoint{bafx::fx::PointF{128.5F, 128.25F}, 0.0F},
                bafx::fx::TrailPoint{bafx::fx::PointF{168.75F, 143.5F}, 0.0F},
            },
            8.0F}};
    return snapshot;
}

[[nodiscard]] bafx::fx::FrameSnapshot makeDiskAndTrailSnapshot()
{
    bafx::fx::FrameSnapshot snapshot = makeDiskSnapshot(true);
    snapshot.trailStrokes = makeTwoTrailSnapshot().trailStrokes;
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
        true});
    return snapshot;
}

[[nodiscard]] bafx::fx::FrameSnapshot makeDissolveRingSnapshot()
{
    bafx::fx::FrameSnapshot snapshot{};
    snapshot.active = true;
    snapshot.sprites.push_back(bafx::fx::Sprite{
        bafx::fx::SpriteKind::DissolveRing,
        bafx::fx::PointF{
            static_cast<float>(testSize.width) * 0.5F,
            static_cast<float>(testSize.height) * 0.5F},
        112.0F,
        0.0F,
        bafx::fx::ColorF{1.0F, 1.0F, 1.0F, 1.0F},
        5.992157F,
        0.5F,
        0U,
        4550,
        false});
    return snapshot;
}

[[nodiscard]] bafx::fx::FrameSnapshot makeTriangleTransportSnapshot(
    const float particleAlpha,
    const float artisticIntensity)
{
    bafx::fx::FrameSnapshot snapshot{};
    snapshot.active = true;
    snapshot.sprites.push_back(bafx::fx::Sprite{
        bafx::fx::SpriteKind::Triangle,
        bafx::fx::PointF{
            static_cast<float>(testSize.width) * 0.5F,
            static_cast<float>(testSize.height) * 0.25F},
        64.0F,
        0.0F,
        bafx::fx::ColorF{1.0F, 1.0F, 1.0F, particleAlpha},
        artisticIntensity,
        0.0F,
        0U,
        4550,
        true});
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
        true});
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
        true});
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

[[nodiscard]] std::array<float, 3U> maximumRgbChannelsInBox(
    const std::vector<ReadbackPixel>& pixels,
    const std::uint32_t left,
    const std::uint32_t top,
    const std::uint32_t right,
    const std::uint32_t bottom) noexcept
{
    std::array<float, 3U> maximum{};
    for (std::uint32_t y = top; y < bottom; ++y)
    {
        for (std::uint32_t x = left; x < right; ++x)
        {
            const ReadbackPixel& pixel =
                pixels[static_cast<std::size_t>(y) * testSize.width + x];
            maximum[0] = (std::max)(maximum[0], pixel.red);
            maximum[1] = (std::max)(maximum[1], pixel.green);
            maximum[2] = (std::max)(maximum[2], pixel.blue);
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
    const bool enforceFxOnlyAlphaLimit = true,
    const bool allowAdditiveRgb = false)
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
        if (allowAdditiveRgb)
        {
            // Background-aware output follows the WebGL2/WebGPU coverage
            // contract: direct emission and Bloom may exceed Coverage Alpha.
            continue;
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

[[nodiscard]] float linearToSrgbChannel(const float value) noexcept
{
    const float linear = std::clamp(value, 0.0F, 1.0F);
    if (linear <= 0.0031308F)
    {
        return linear * 12.92F;
    }
    return 1.055F * std::pow(linear, 1.0F / 2.4F) - 0.055F;
}

[[nodiscard]] std::uint8_t linearToSrgb8(const float value) noexcept
{
    return static_cast<std::uint8_t>(std::lround(
        linearToSrgbChannel(value) * 255.0F));
}

[[nodiscard]] std::uint8_t obsSdrRolloff8(
    const float value,
    const float peak) noexcept
{
    const float encoded = std::max(value, 0.0F) / (1.0F + std::max(peak, 0.0F));
    return static_cast<std::uint8_t>(std::lround(
        std::clamp(encoded, 0.0F, 1.0F) * 255.0F));
}

void checkObsSdrRolloffEncoding(
    const Bgra8Image& encoded,
    const Rgba16FloatImage& direct,
    const Rgba16FloatImage* const bloom = nullptr)
{
    BAFX_CHECK(encoded.width == direct.width);
    BAFX_CHECK(encoded.height == direct.height);
    BAFX_CHECK(encoded.pixels.size() == direct.pixels.size());
    if (bloom != nullptr)
    {
        BAFX_CHECK(bloom->width == direct.width);
        BAFX_CHECK(bloom->height == direct.height);
        BAFX_CHECK(bloom->pixels.size() == direct.pixels.size());
    }

    constexpr float minimumEmission = 0.01F;
    constexpr float maximumEmission = 0.90F;
    constexpr int byteTolerance = 2;
    std::size_t checkedPixels = 0U;
    std::size_t preventedSrgbUpliftPixels = 0U;
    for (std::size_t index = 0U; index < direct.pixels.size(); ++index)
    {
        const Rgba16FloatPixel directPixel = direct.pixels[index];
        const Rgba16FloatPixel bloomPixel = bloom == nullptr
            ? Rgba16FloatPixel{}
            : bloom->pixels[index];
        const std::array<float, 3U> linear{
            std::max(
                halfToFloat(directPixel.red) + halfToFloat(bloomPixel.red),
                0.0F),
            std::max(
                halfToFloat(directPixel.green) + halfToFloat(bloomPixel.green),
                0.0F),
            std::max(
                halfToFloat(directPixel.blue) + halfToFloat(bloomPixel.blue),
                0.0F)};
        const float peak = std::max({linear[0], linear[1], linear[2]});
        if (peak < minimumEmission || peak > maximumEmission)
        {
            continue;
        }

        const Bgra8UnormPixel actualPixel = encoded.pixels[index];
        const std::array<std::uint8_t, 3U> actual{
            actualPixel.red,
            actualPixel.green,
            actualPixel.blue};
        for (std::size_t channel = 0U; channel < actual.size(); ++channel)
        {
            // OBS adds this byte-domain delta to an encoded game frame. Derive
            // the expected hue-preserving shoulder from captured FP16 layers.
            const int expected = obsSdrRolloff8(linear[channel], peak);
            const int delta = static_cast<int>(actual[channel]) - expected;
            BAFX_CHECK(delta >= -byteTolerance && delta <= byteTolerance);
        }
        const bool preventsSrgbUplift =
            static_cast<int>(linearToSrgb8(peak))
                - static_cast<int>(obsSdrRolloff8(peak, peak))
            >= 16;
        if (preventsSrgbUplift)
        {
            ++preventedSrgbUpliftPixels;
        }
        ++checkedPixels;
    }
    BAFX_CHECK(checkedPixels > 100U);
    BAFX_CHECK(preventedSrgbUpliftPixels > 100U);
}

[[nodiscard]] float srgbToLinearChannel(const float value) noexcept
{
    const float srgb = std::clamp(value, 0.0F, 1.0F);
    if (srgb <= 0.04045F)
    {
        return srgb / 12.92F;
    }
    return std::pow((srgb + 0.055F) / 1.055F, 2.4F);
}

[[nodiscard]] float smoothstepReference(
    const float lower,
    const float upper,
    const float value) noexcept
{
    const float normalized = std::clamp(
        (value - lower) / (upper - lower),
        0.0F,
        1.0F);
    return normalized * normalized * (3.0F - 2.0F * normalized);
}

[[nodiscard]] ReadbackPixel recordingCompatibleWebReference(
    const ReadbackPixel direct) noexcept
{
    constexpr float alphaLimit = 0.90F;
    constexpr float compensationMix = 0.35F;
    const float alpha = std::min(
        std::clamp(direct.alpha, 0.0F, 1.0F),
        alphaLimit);
    if (alpha <= 1.0e-6F)
    {
        return {};
    }

    std::array<float, 3U> premultipliedSrgb{
        linearToSrgbChannel(direct.red),
        linearToSrgbChannel(direct.green),
        linearToSrgbChannel(direct.blue)};
    const float maximumSrgb = std::max({
        premultipliedSrgb[0],
        premultipliedSrgb[1],
        premultipliedSrgb[2]});
    const float capacityScale = std::min(
        1.0F,
        alpha / std::max(maximumSrgb, 1.0e-6F));
    for (float& channel : premultipliedSrgb)
    {
        channel *= capacityScale;
    }

    const float maximumPremultiplied = std::max({
        premultipliedSrgb[0],
        premultipliedSrgb[1],
        premultipliedSrgb[2]});
    const float energyRatio = maximumPremultiplied
        / std::max(alpha, 1.0e-6F);
    const float gate = smoothstepReference(0.25F, 0.75F, energyRatio)
        * smoothstepReference(0.03125F, 0.25F, maximumPremultiplied);
    for (float& channel : premultipliedSrgb)
    {
        channel += (maximumPremultiplied - channel)
            * compensationMix
            * gate;
    }

    return ReadbackPixel{
        srgbToLinearChannel(premultipliedSrgb[0] / alpha) * alpha,
        srgbToLinearChannel(premultipliedSrgb[1] / alpha) * alpha,
        srgbToLinearChannel(premultipliedSrgb[2] / alpha) * alpha,
        alpha};
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

[[nodiscard]] float maximumAlphaDeltaInBox(
    const std::vector<ReadbackPixel>& left,
    const std::vector<ReadbackPixel>& right,
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
            maximum = (std::max)(
                maximum,
                std::abs(left[index].alpha - right[index].alpha));
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

BAFX_TEST(warp_theme_color_changes_every_native_effect_layer)
{
    ComApartment apartment;
    const WarpDevice graphics = createWarpDevice();
    FxGpuRenderer renderer(graphics.device.Get(), graphics.context.Get(), testSize);
    renderer.setBloomSettings(FxBloomSettings{0.0F, 7.0F});

    struct LayerCase final
    {
        bafx::fx::FrameSnapshot snapshot{};
        std::uint32_t left{0U};
        std::uint32_t top{0U};
        std::uint32_t right{0U};
        std::uint32_t bottom{0U};
    };
    LayerCase trailCase{makeTwoTrailSnapshot(), 16U, 48U, 120U, 80U};
    LayerCase diskCase{makeDiskSnapshot(false), 96U, 96U, 160U, 160U};
    LayerCase ringCase{makeDissolveRingSnapshot(), 64U, 64U, 192U, 192U};
    LayerCase triangleCase{makeTriangleSnapshot(), 96U, 96U, 160U, 160U};
    const bafx::fx::ColorF baseBlueColor{
        srgbToLinearChannel(76.0F / 255.0F),
        srgbToLinearChannel(167.0F / 255.0F),
        1.0F,
        1.0F};
    diskCase.snapshot.sprites.front().color = baseBlueColor;
    ringCase.snapshot.sprites.front().color = baseBlueColor;
    triangleCase.snapshot.sprites.front().color = baseBlueColor;

    for (const LayerCase& layer : {trailCase, diskCase, ringCase, triangleCase})
    {
        const RenderTarget defaultTarget = createRenderTarget(
            graphics.device.Get());
        renderer.setThemeColor("#4ca7ff");
        renderer.render(layer.snapshot, defaultTarget.view.Get());
        const std::array<float, 3U> defaultChannels = maximumRgbChannelsInBox(
            readback(graphics.context.Get(), defaultTarget.texture.Get()),
            layer.left,
            layer.top,
            layer.right,
            layer.bottom);

        const RenderTarget redTarget = createRenderTarget(graphics.device.Get());
        renderer.setThemeColor("#ff0000");
        renderer.render(layer.snapshot, redTarget.view.Get());
        const std::array<float, 3U> redChannels = maximumRgbChannelsInBox(
            readback(graphics.context.Get(), redTarget.texture.Get()),
            layer.left,
            layer.top,
            layer.right,
            layer.bottom);
        BAFX_CHECK(defaultChannels[2] > 1.0e-3F);
        BAFX_CHECK(redChannels[0] > redChannels[2]);
        BAFX_CHECK(redChannels[0] > 1.0e-3F);
    }
}

BAFX_TEST(warp_theme_coverage_scale_only_limits_unknown_background_alpha)
{
    ComApartment apartment;
    const WarpDevice graphics = createWarpDevice();
    FxGpuRenderer renderer(graphics.device.Get(), graphics.context.Get(), testSize);
    const bafx::fx::FrameSnapshot snapshot = makeDiskAndTrailSnapshot();

    renderer.setThemeColor("#4ca7ff");
    const RenderTarget defaultTarget = createRenderTarget(graphics.device.Get());
    renderer.render(snapshot, defaultTarget.view.Get());
    const std::vector<ReadbackPixel> defaultPixels = readback(
        graphics.context.Get(),
        defaultTarget.texture.Get());

    renderer.setThemeColor("#000001");
    const RenderTarget nearBlackTarget = createRenderTarget(graphics.device.Get());
    renderer.render(snapshot, nearBlackTarget.view.Get());
    const std::vector<ReadbackPixel> nearBlackPixels = readback(
        graphics.context.Get(),
        nearBlackTarget.texture.Get());
    BAFX_CHECK(
        maximumAlphaInBox(nearBlackPixels, 96U, 96U, 160U, 160U)
        < maximumAlphaInBox(defaultPixels, 96U, 96U, 160U, 160U) / 200.0F);

    const RenderTarget background = createRenderTarget(graphics.device.Get());
    constexpr std::array<float, 4U> backgroundColor{0.25F, 0.5F, 0.75F, 1.0F};
    graphics.context->ClearRenderTargetView(background.view.Get(), backgroundColor.data());
    renderer.setThemeColor("#4ca7ff");
    const RenderTarget awareDefaultTarget = createRenderTarget(graphics.device.Get());
    renderer.render(
        snapshot,
        awareDefaultTarget.view.Get(),
        BackgroundRenderInput{background.shaderResource.Get()});
    const std::vector<ReadbackPixel> awareDefault = readback(
        graphics.context.Get(),
        awareDefaultTarget.texture.Get());
    renderer.setThemeColor("#000001");
    const RenderTarget awareNearBlackTarget = createRenderTarget(graphics.device.Get());
    renderer.render(
        snapshot,
        awareNearBlackTarget.view.Get(),
        BackgroundRenderInput{background.shaderResource.Get()});
    const std::vector<ReadbackPixel> awareNearBlack = readback(
        graphics.context.Get(),
        awareNearBlackTarget.texture.Get());
    BAFX_CHECK(maximumRgbaDelta(awareDefault, awareNearBlack) > 1.0e-3F);
    BAFX_CHECK(
        maximumAlphaInBox(awareNearBlack, 96U, 96U, 160U, 160U)
        > maximumAlphaInBox(nearBlackPixels, 96U, 96U, 160U, 160U) * 10.0F
            + 0.01F);
}

BAFX_TEST(warp_light_background_profile_uses_visual_max_source_over_capacity)
{
    ComApartment apartment;
    const WarpDevice graphics = createWarpDevice();
    FxGpuRenderer renderer(graphics.device.Get(), graphics.context.Get(), testSize);
    const bafx::fx::FrameSnapshot snapshot = makeDiskSnapshot(true);

    renderer.setOverlayProfile(FxOverlayProfile::FxOnlyFallback);
    const RenderTarget fallbackTarget = createRenderTarget(graphics.device.Get());
    renderer.render(snapshot, fallbackTarget.view.Get());
    const std::vector<ReadbackPixel> fallback = readback(
        graphics.context.Get(),
        fallbackTarget.texture.Get());

    renderer.setOverlayProfile(FxOverlayProfile::LightBackground);
    const RenderTarget lightTarget = createRenderTarget(graphics.device.Get());
    renderer.render(snapshot, lightTarget.view.Get());
    const std::vector<ReadbackPixel> light = readback(
        graphics.context.Get(),
        lightTarget.texture.Get());

    checkValidDesktopPremultiplied(fallback);
    checkValidDesktopPremultiplied(light);
    const std::size_t center = static_cast<std::size_t>(testSize.height / 2U)
        * testSize.width
        + testSize.width / 2U;
    BAFX_CHECK(fallback[center].alpha > 0.9F);
    BAFX_CHECK(light[center].alpha > 0.8F);
    BAFX_CHECK(light[center].alpha <= 0.851F);
    BAFX_CHECK(maximumRgbaDelta(fallback, light) > 0.05F);
}

BAFX_TEST(warp_recording_compatible_profile_matches_web_overlay_preset)
{
    ComApartment apartment;
    const WarpDevice graphics = createWarpDevice();
    FxGpuRenderer renderer(graphics.device.Get(), graphics.context.Get(), testSize);
    const bafx::fx::FrameSnapshot snapshot = makeDiskSnapshot(true);

    renderer.setOverlayProfile(FxOverlayProfile::RecordingCompatible);
    const RenderTarget target = createRenderTarget(graphics.device.Get());
    renderer.render(snapshot, target.view.Get());
    const std::vector<ReadbackPixel> pixels = readback(
        graphics.context.Get(),
        target.texture.Get());

    checkValidDesktopPremultiplied(pixels);
    constexpr float recordingAlphaLimit = 0.90F;
    constexpr float tolerance = 2.0e-3F;
    for (const ReadbackPixel& pixel : pixels)
    {
        BAFX_CHECK(pixel.alpha <= recordingAlphaLimit + tolerance);
        BAFX_CHECK(pixel.red <= pixel.alpha + tolerance);
        BAFX_CHECK(pixel.green <= pixel.alpha + tolerance);
        BAFX_CHECK(pixel.blue <= pixel.alpha + tolerance);
    }

    const std::size_t center = static_cast<std::size_t>(testSize.height / 2U)
        * testSize.width
        + testSize.width / 2U;
    BAFX_CHECK_NEAR(
        pixels[center].alpha,
        recordingAlphaLimit,
        tolerance);
}

BAFX_TEST(warp_recording_compatible_converts_web_srgb_payload_to_scrgb)
{
    ComApartment apartment;
    const WarpDevice graphics = createWarpDevice();
    FxGpuRenderer renderer(graphics.device.Get(), graphics.context.Get(), testSize);
    bafx::fx::FrameSnapshot snapshot = makeDiskTransportSnapshot(0.55F, 0.45F);
    snapshot.sprites.front().color = bafx::fx::ColorF{
        0.15F,
        0.50F,
        1.00F,
        0.55F};

    const RenderTarget captureTarget = createRenderTarget(graphics.device.Get());
    const FxGpuFrameCapture capture = renderer.renderAndCapture(
        snapshot,
        captureTarget.view.Get());
    const std::vector<ReadbackPixel> direct = toFloatPixels(capture.directSurface);

    renderer.setOverlayProfile(FxOverlayProfile::RecordingCompatible);
    const RenderTarget outputTarget = createRenderTarget(graphics.device.Get());
    renderer.render(snapshot, outputTarget.view.Get());
    const std::vector<ReadbackPixel> output = readback(
        graphics.context.Get(),
        outputTarget.texture.Get());

    const std::size_t center = static_cast<std::size_t>(testSize.height / 2U)
        * testSize.width
        + testSize.width / 2U;
    const ReadbackPixel expected = recordingCompatibleWebReference(direct[center]);
    constexpr float tolerance = 3.0e-3F;
    BAFX_CHECK_NEAR(output[center].red, expected.red, tolerance);
    BAFX_CHECK_NEAR(output[center].green, expected.green, tolerance);
    BAFX_CHECK_NEAR(output[center].blue, expected.blue, tolerance);
    BAFX_CHECK_NEAR(output[center].alpha, expected.alpha, tolerance);

    // The old path compressed directly in linear space. The Web-compatible
    // transfer keeps the selected transparent-window color visibly brighter.
    BAFX_CHECK(output[center].blue > direct[center].blue + 0.05F);
}

BAFX_TEST(warp_background_path_uses_full_differential_bloom)
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

    const RenderTarget backgroundAwareTarget =
        createRenderTarget(graphics.device.Get());
    renderer.render(
        snapshot,
        backgroundAwareTarget.view.Get(),
        BackgroundRenderInput{background.shaderResource.Get()});
    const std::vector<ReadbackPixel> backgroundAware = readback(
        graphics.context.Get(),
        backgroundAwareTarget.texture.Get());

    checkValidDesktopPremultiplied(fxOnly);
    checkValidDesktopPremultiplied(backgroundAware, false, true);
    BAFX_CHECK(maximumRgbaDelta(fxOnly, backgroundAware) > 1.0e-3F);

    const std::size_t center = static_cast<std::size_t>(testSize.height / 2U)
        * testSize.width
        + testSize.width / 2U;
    BAFX_CHECK(backgroundAware[center].blue > 0.5F);
}

BAFX_TEST(warp_sdr_background_white_preserves_unity_working_space)
{
    ComApartment apartment;
    const WarpDevice graphics = createWarpDevice();
    const bafx::fx::FrameSnapshot snapshot = makeDiskSnapshot(true);

    CompositionOutputMapping referenceMapping{};
    CompositionOutputMapping physicalMapping{};
    physicalMapping.backgroundReferenceWhiteNits = 200.0F;
    physicalMapping.backgroundReferenceWhiteValid = true;
    FxGpuRenderer referenceRenderer(
        graphics.device.Get(),
        graphics.context.Get(),
        testSize,
        FxBloomSettings{},
        referenceMapping);
    FxGpuRenderer physicalRenderer(
        graphics.device.Get(),
        graphics.context.Get(),
        testSize,
        FxBloomSettings{},
        physicalMapping);

    const RenderTarget referenceBackground = createRenderTarget(
        graphics.device.Get());
    const RenderTarget physicalBackground = createRenderTarget(
        graphics.device.Get());
    constexpr std::array<float, 4> referenceColor{0.2F, 0.4F, 0.7F, 1.0F};
    constexpr float physicalWhiteScale = 200.0F / 80.0F;
    constexpr std::array<float, 4> physicalColor{
        referenceColor[0] * physicalWhiteScale,
        referenceColor[1] * physicalWhiteScale,
        referenceColor[2] * physicalWhiteScale,
        1.0F};
    graphics.context->ClearRenderTargetView(
        referenceBackground.view.Get(),
        referenceColor.data());
    graphics.context->ClearRenderTargetView(
        physicalBackground.view.Get(),
        physicalColor.data());

    const RenderTarget referenceTarget = createRenderTarget(
        graphics.device.Get());
    const RenderTarget physicalTarget = createRenderTarget(
        graphics.device.Get());
    referenceRenderer.render(
        snapshot,
        referenceTarget.view.Get(),
        BackgroundRenderInput{referenceBackground.shaderResource.Get()});
    physicalRenderer.render(
        snapshot,
        physicalTarget.view.Get(),
        BackgroundRenderInput{physicalBackground.shaderResource.Get()});
    const std::vector<ReadbackPixel> reference = readback(
        graphics.context.Get(),
        referenceTarget.texture.Get());
    const std::vector<ReadbackPixel> physical = readback(
        graphics.context.Get(),
        physicalTarget.texture.Get());

    // The WGC frame is physical scRGB, but both Bloom and the BGRA8 final pass
    // must observe the same Unity-relative background as the 80-nit baseline.
    BAFX_CHECK(maximumRgbaDelta(reference, physical) <= 5.0e-3F);
}

BAFX_TEST(warp_usable_background_age_never_modulates_click_or_trail)
{
    ComApartment apartment;
    const WarpDevice graphics = createWarpDevice();
    FxGpuRenderer renderer(graphics.device.Get(), graphics.context.Get(), testSize);
    const bafx::fx::FrameSnapshot snapshot = makeDiskAndTrailSnapshot();
    constexpr bafx::core::BackgroundUsagePolicy usagePolicy{100ms, 48ms, 7U};
    constexpr std::array<bafx::core::MonotonicTime, 4U> capturedAt{
        0ms,
        -50ms,
        -100ms + 1ns,
        48ms};
    constexpr std::array<std::array<float, 4U>, 2U> backgroundColors{
        std::array<float, 4U>{0.02F, 0.04F, 0.08F, 1.0F},
        std::array<float, 4U>{0.95F, 0.95F, 0.95F, 1.0F}};

    for (const std::array<float, 4U>& backgroundColor : backgroundColors)
    {
        const RenderTarget background = createRenderTarget(graphics.device.Get());
        graphics.context->ClearRenderTargetView(
            background.view.Get(),
            backgroundColor.data());

        std::vector<ReadbackPixel> reference;
        for (const bafx::core::MonotonicTime captureTime : capturedAt)
        {
            const bafx::core::BackgroundUsageDecision usage =
                bafx::core::evaluateBackgroundUsage(
                    bafx::core::BackgroundFrameStamp{
                        captureTime,
                        7U,
                        true,
                        true},
                    0ms,
                    usagePolicy);
            BAFX_CHECK(usage.status == bafx::core::BackgroundUsageStatus::Usable);
            BAFX_CHECK(usage.enabled);

            const RenderTarget target = createRenderTarget(graphics.device.Get());
            renderer.render(
                snapshot,
                target.view.Get(),
                BackgroundRenderInput{background.shaderResource.Get()});
            const std::vector<ReadbackPixel> pixels = readback(
                graphics.context.Get(),
                target.texture.Get());
            checkValidDesktopPremultiplied(pixels, false, true);
            BAFX_CHECK(maximumRgbInBox(pixels, 96U, 96U, 160U, 160U) > 1.0e-3F);
            BAFX_CHECK(maximumRgbInBox(pixels, 16U, 48U, 120U, 80U) > 1.0e-3F);

            if (reference.empty())
            {
                reference = pixels;
            }
            else
            {
                BAFX_CHECK(maximumRgbaDelta(reference, pixels) == 0.0F);
            }
        }
    }
}

BAFX_TEST(warp_latched_background_path_stays_stable_at_age_boundaries)
{
    ComApartment apartment;
    const WarpDevice graphics = createWarpDevice();
    FxGpuRenderer renderer(graphics.device.Get(), graphics.context.Get(), testSize);
    const bafx::fx::FrameSnapshot snapshot = makeDiskAndTrailSnapshot();
    constexpr bafx::core::BackgroundUsagePolicy acquirePolicy{100ms, 48ms, 7U};
    constexpr bafx::core::BackgroundUsagePolicy retainPolicy{250ms, 48ms, 7U};
    constexpr std::array<std::array<float, 4U>, 2U> backgroundColors{
        std::array<float, 4U>{0.02F, 0.04F, 0.08F, 1.0F},
        std::array<float, 4U>{0.95F, 0.95F, 0.95F, 1.0F}};

    for (const std::array<float, 4U>& backgroundColor : backgroundColors)
    {
        const RenderTarget background = createRenderTarget(graphics.device.Get());
        graphics.context->ClearRenderTargetView(
            background.view.Get(),
            backgroundColor.data());

        const auto renderSelected = [&](
            bafx::core::BackgroundPathLatch& latch,
            const bafx::core::MonotonicTime capturedAt,
            const bafx::core::MonotonicTime renderAt,
            const bafx::core::BackgroundRenderPath expectedPath)
        {
            const bafx::core::BackgroundFrameStamp stamp{
                capturedAt,
                7U,
                true,
                true};
            const bafx::core::BackgroundUsageDecision acquire =
                bafx::core::evaluateBackgroundUsage(
                    stamp,
                    renderAt,
                    acquirePolicy);
            const bafx::core::BackgroundUsageDecision retain =
                bafx::core::evaluateBackgroundUsage(
                    stamp,
                    renderAt,
                    retainPolicy);
            const bafx::core::BackgroundRenderPath path = latch.select(
                snapshot.hasDrawableContent(),
                acquire.enabled,
                retain.enabled);
            BAFX_CHECK(path == expectedPath);

            const std::optional<BackgroundRenderInput> input =
                path == bafx::core::BackgroundRenderPath::BackgroundAware
                ? std::optional<BackgroundRenderInput>{
                    BackgroundRenderInput{background.shaderResource.Get()}}
                : std::nullopt;
            const RenderTarget target = createRenderTarget(graphics.device.Get());
            renderer.render(snapshot, target.view.Get(), input);
            const std::vector<ReadbackPixel> pixels = readback(
                graphics.context.Get(),
                target.texture.Get());
            checkValidDesktopPremultiplied(pixels, false, true);
            return pixels;
        };

        bafx::core::BackgroundPathLatch backgroundAwareLatch;
        const std::vector<ReadbackPixel> beforeAcquireBoundary = renderSelected(
            backgroundAwareLatch,
            -100ms + 1ns,
            0ms,
            bafx::core::BackgroundRenderPath::BackgroundAware);
        const std::vector<ReadbackPixel> atAcquireBoundary = renderSelected(
            backgroundAwareLatch,
            -100ms,
            0ms,
            bafx::core::BackgroundRenderPath::BackgroundAware);
        const std::vector<ReadbackPixel> recoveredBeforeRetainExpiry =
            renderSelected(
                backgroundAwareLatch,
                0ms,
                0ms,
                bafx::core::BackgroundRenderPath::BackgroundAware);
        BAFX_CHECK(maximumRgbaDelta(
            beforeAcquireBoundary,
            atAcquireBoundary) == 0.0F);
        BAFX_CHECK(maximumRgbaDelta(
            beforeAcquireBoundary,
            recoveredBeforeRetainExpiry) == 0.0F);

        const std::vector<ReadbackPixel> atRetainBoundary = renderSelected(
            backgroundAwareLatch,
            -250ms,
            0ms,
            bafx::core::BackgroundRenderPath::FxOnly);
        const std::vector<ReadbackPixel> recoveredAfterRetainExpiry =
            renderSelected(
                backgroundAwareLatch,
                0ms,
                0ms,
                bafx::core::BackgroundRenderPath::FxOnly);
        BAFX_CHECK(maximumRgbaDelta(
            atRetainBoundary,
            recoveredAfterRetainExpiry) == 0.0F);

        BAFX_CHECK(backgroundAwareLatch.select(false, false, false)
            == bafx::core::BackgroundRenderPath::FxOnly);
        const std::vector<ReadbackPixel> nextBatch = renderSelected(
            backgroundAwareLatch,
            0ms,
            0ms,
            bafx::core::BackgroundRenderPath::BackgroundAware);
        BAFX_CHECK(maximumRgbaDelta(
            beforeAcquireBoundary,
            nextBatch) == 0.0F);

        bafx::core::BackgroundPathLatch fxOnlyLatch;
        const std::vector<ReadbackPixel> staleFirstFrame = renderSelected(
            fxOnlyLatch,
            -100ms,
            0ms,
            bafx::core::BackgroundRenderPath::FxOnly);
        const std::vector<ReadbackPixel> recoveredFxOnly = renderSelected(
            fxOnlyLatch,
            0ms,
            0ms,
            bafx::core::BackgroundRenderPath::FxOnly);
        BAFX_CHECK(maximumRgbaDelta(
            staleFirstFrame,
            recoveredFxOnly) == 0.0F);
    }
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
            BackgroundRenderInput{background.shaderResource.Get()});
        return readback(graphics.context.Get(), output.texture.Get());
    };

    constexpr std::array<float, 4> darkBackground{0.1F, 0.2F, 0.3F, 1.0F};
    constexpr std::array<float, 4> lightBackground{0.7F, 0.6F, 0.5F, 1.0F};
    const std::vector<ReadbackPixel> overDark = renderOver(darkBackground);
    const std::vector<ReadbackPixel> overLight = renderOver(lightBackground);
    checkValidDesktopPremultiplied(overDark, false, true);
    checkValidDesktopPremultiplied(overLight, false, true);

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

BAFX_TEST(warp_background_transport_suppresses_near_white_capture_noise)
{
    ComApartment apartment;
    const WarpDevice graphics = createWarpDevice();
    FxGpuRenderer renderer(graphics.device.Get(), graphics.context.Get(), testSize);
    renderer.setBloomSettings(FxBloomSettings{0.0F, 7.0F});
    const bafx::fx::FrameSnapshot snapshot = makeDiskAndTrailSnapshot();
    const auto renderOver = [&](const std::array<float, 4>& color)
    {
        const RenderTarget background = createRenderTarget(graphics.device.Get());
        graphics.context->ClearRenderTargetView(background.view.Get(), color.data());
        const RenderTarget output = createRenderTarget(graphics.device.Get());
        renderer.render(
            snapshot,
            output.view.Get(),
            BackgroundRenderInput{background.shaderResource.Get()});
        const std::vector<ReadbackPixel> pixels = readback(
            graphics.context.Get(),
            output.texture.Get());
        return pixels;
    };

    constexpr std::array<float, 4> nearWhiteBackground{
        0.99951171875F,
        0.99951171875F,
        0.99951171875F,
        1.0F};
    constexpr std::array<float, 4> pureWhiteBackground{
        1.0F,
        1.0F,
        1.0F,
        1.0F};
    constexpr std::array<float, 4> visibleLightBackground{
        0.99F,
        0.99F,
        0.99F,
        1.0F};

    const std::vector<ReadbackPixel> nearWhite = renderOver(nearWhiteBackground);
    const std::vector<ReadbackPixel> pureWhite = renderOver(pureWhiteBackground);
    const std::vector<ReadbackPixel> visibleLight =
        renderOver(visibleLightBackground);
    checkValidDesktopPremultiplied(nearWhite, false, true);
    checkValidDesktopPremultiplied(pureWhite, false, true);
    checkValidDesktopPremultiplied(visibleLight, false, true);

    constexpr std::uint32_t diskLeft = 96U;
    constexpr std::uint32_t diskTop = 96U;
    constexpr std::uint32_t diskRight = 160U;
    constexpr std::uint32_t diskBottom = 160U;
    constexpr std::uint32_t trailLeft = 16U;
    constexpr std::uint32_t trailTop = 48U;
    constexpr std::uint32_t trailRight = 120U;
    constexpr std::uint32_t trailBottom = 80U;

    BAFX_CHECK_NEAR(
        maximumAlphaInBox(nearWhite, diskLeft, diskTop, diskRight, diskBottom),
        maximumAlphaInBox(pureWhite, diskLeft, diskTop, diskRight, diskBottom),
        2.0e-3F);
    BAFX_CHECK_NEAR(
        maximumAlphaInBox(nearWhite, trailLeft, trailTop, trailRight, trailBottom),
        maximumAlphaInBox(pureWhite, trailLeft, trailTop, trailRight, trailBottom),
        2.0e-3F);

    // Compare the payloads over the same desktop sample. This catches a
    // transparent-to-opaque jump even when the two captured backgrounds differ
    // by only one FP16 step.
    const float diskNoiseDelta = std::max(
        maximumCompositeRgbDeltaInBox(
            nearWhite,
            pureWhite,
            nearWhiteBackground,
            diskLeft,
            diskTop,
            diskRight,
            diskBottom),
        maximumCompositeRgbDeltaInBox(
            nearWhite,
            pureWhite,
            pureWhiteBackground,
            diskLeft,
            diskTop,
            diskRight,
            diskBottom));
    const float trailNoiseDelta = std::max(
        maximumCompositeRgbDeltaInBox(
            nearWhite,
            pureWhite,
            nearWhiteBackground,
            trailLeft,
            trailTop,
            trailRight,
            trailBottom),
        maximumCompositeRgbDeltaInBox(
            nearWhite,
            pureWhite,
            pureWhiteBackground,
            trailLeft,
            trailTop,
            trailRight,
            trailBottom));
    BAFX_CHECK(diskNoiseDelta <= 2.0e-3F);
    BAFX_CHECK(trailNoiseDelta <= 2.0e-3F);

    BAFX_CHECK(
        maximumAlphaInBox(visibleLight, diskLeft, diskTop, diskRight, diskBottom)
        > 0.5F);
    BAFX_CHECK(
        maximumAlphaInBox(
            visibleLight,
            trailLeft,
            trailTop,
            trailRight,
            trailBottom)
        > 0.5F);
}

BAFX_TEST(warp_background_transport_follows_authored_coverage_continuously)
{
    ComApartment apartment;
    const WarpDevice graphics = createWarpDevice();
    FxGpuRenderer renderer(graphics.device.Get(), graphics.context.Get(), testSize);
    renderer.setBloomSettings(FxBloomSettings{0.0F, 7.0F});

    const RenderTarget background = createRenderTarget(graphics.device.Get());
    constexpr std::array<float, 4> lightBackground{
        0.99F,
        0.99F,
        0.99F,
        1.0F};
    graphics.context->ClearRenderTargetView(
        background.view.Get(),
        lightBackground.data());

    constexpr std::array<float, 5> intensities{
        0.0975F,
        0.0985F,
        0.0995F,
        0.1005F,
        0.1015F};
    constexpr std::uint32_t centerX = testSize.width / 2U;
    constexpr std::uint32_t centerY = testSize.height / 2U;
    const std::size_t centerIndex = static_cast<std::size_t>(centerY)
        * testSize.width
        + centerX;

    std::optional<float> previousAlpha;
    float minimumAlpha = 1.0F;
    float maximumAlpha = 0.0F;
    for (const float intensity : intensities)
    {
        const RenderTarget target = createRenderTarget(graphics.device.Get());
        renderer.render(
            makeDiskTransportSnapshot(0.1F, intensity),
            target.view.Get(),
            BackgroundRenderInput{background.shaderResource.Get()});
        const std::vector<ReadbackPixel> pixels = readback(
            graphics.context.Get(),
            target.texture.Get());
        checkValidDesktopPremultiplied(pixels, false, true);

        const float alpha = pixels[centerIndex].alpha;
        minimumAlpha = (std::min)(minimumAlpha, alpha);
        maximumAlpha = (std::max)(maximumAlpha, alpha);
        if (previousAlpha.has_value())
        {
            // A small authored intensity change must not turn a fading click
            // edge from transparent into a full transport layer.
            BAFX_CHECK(std::abs(alpha - *previousAlpha) <= 2.0e-2F);
        }
        previousAlpha = alpha;
    }

    BAFX_CHECK(maximumAlpha > 0.05F);
    BAFX_CHECK(maximumAlpha - minimumAlpha <= 2.0e-2F);
}

BAFX_TEST(warp_background_transport_stays_stable_across_light_capture_steps)
{
    ComApartment apartment;
    const WarpDevice graphics = createWarpDevice();
    FxGpuRenderer renderer(graphics.device.Get(), graphics.context.Get(), testSize);
    renderer.setBloomSettings(FxBloomSettings{0.0F, 7.0F});
    const bafx::fx::FrameSnapshot snapshot = makeDiskAndTrailSnapshot();

    const auto renderOverCapturedBackground = [&](const float capturedValue)
    {
        const RenderTarget background = createRenderTarget(graphics.device.Get());
        const std::array<float, 4> color{
            capturedValue,
            capturedValue,
            capturedValue,
            1.0F};
        graphics.context->ClearRenderTargetView(background.view.Get(), color.data());
        const RenderTarget output = createRenderTarget(graphics.device.Get());
        renderer.render(
            snapshot,
            output.view.Get(),
            BackgroundRenderInput{background.shaderResource.Get()});
        return readback(graphics.context.Get(), output.texture.Get());
    };

    constexpr std::array<float, 5> capturedValues{
        0.98828125F,
        0.9892578125F,
        0.99F,
        0.99072265625F,
        0.99169921875F};

    std::vector<ReadbackPixel> reference;
    for (const float capturedValue : capturedValues)
    {
        const std::vector<ReadbackPixel> pixels =
            renderOverCapturedBackground(capturedValue);
        checkValidDesktopPremultiplied(pixels, false, true);
        if (reference.empty())
        {
            reference = pixels;
            continue;
        }

        // Alpha is the part that DComp applies to the real desktop. It must
        // remain authored and stable even when WGC reports adjacent FP16
        // values for a visually unchanged light surface.
        const float diskAlphaDelta = maximumAlphaDeltaInBox(
            reference,
            pixels,
            96U,
            96U,
            160U,
            160U);
        const float trailAlphaDelta = maximumAlphaDeltaInBox(
            reference,
            pixels,
            16U,
            48U,
            120U,
            80U);
        BAFX_CHECK(diskAlphaDelta <= 2.0e-3F);
        BAFX_CHECK(trailAlphaDelta <= 2.0e-3F);
    }

    BAFX_CHECK(maximumAlphaInBox(
        reference,
        96U,
        96U,
        160U,
        160U) > 0.5F);
    BAFX_CHECK(maximumAlphaInBox(
        reference,
        16U,
        48U,
        120U,
        80U) > 0.05F);
}

BAFX_TEST(warp_temporal_background_filter_stabilizes_disk_and_trail)
{
    ComApartment apartment;
    const WarpDevice graphics = createWarpDevice();
    FxGpuRenderer renderer(graphics.device.Get(), graphics.context.Get(), testSize);
    renderer.setBloomSettings(FxBloomSettings{0.0F, 7.0F});

    RenderTarget previous = createRenderTarget(graphics.device.Get());
    RenderTarget current = createRenderTarget(graphics.device.Get());
    RenderTarget filtered = createRenderTarget(graphics.device.Get());
    constexpr std::array<float, 4> seedColor{0.99F, 0.99F, 0.99F, 1.0F};
    graphics.context->ClearRenderTargetView(previous.view.Get(), seedColor.data());

    const bafx::fx::FrameSnapshot snapshot = makeDiskAndTrailSnapshot();
    std::vector<ReadbackPixel> reference;
    constexpr std::array<float, 6> jitteredSamples{
        0.9892578125F,
        0.99072265625F,
        0.98974609375F,
        0.990234375F,
        0.9892578125F,
        0.99072265625F};
    for (const float sample : jitteredSamples)
    {
        const std::array<float, 4> color{sample, sample, sample, 1.0F};
        graphics.context->ClearRenderTargetView(current.view.Get(), color.data());
        renderer.stabilizeBackgroundFrame(
            previous.shaderResource.Get(),
            current.shaderResource.Get(),
            filtered.view.Get());
        const RenderTarget output = createRenderTarget(graphics.device.Get());
        renderer.render(
            snapshot,
            output.view.Get(),
            BackgroundRenderInput{filtered.shaderResource.Get()});
        const std::vector<ReadbackPixel> pixels = readback(
            graphics.context.Get(),
            output.texture.Get());
        checkValidDesktopPremultiplied(pixels, false, true);
        if (reference.empty())
        {
            reference = pixels;
        }
        else
        {
            // Both the Cross2 disk and the additive trail must see the same
            // stable background; adjacent WGC FP16 steps cannot flash either.
            BAFX_CHECK(maximumRgbaDelta(reference, pixels) <= 2.0e-3F);
        }
        std::swap(previous, filtered);
    }

    const std::array<float, 4> changedColor{0.90F, 0.90F, 0.90F, 1.0F};
    graphics.context->ClearRenderTargetView(current.view.Get(), changedColor.data());
    renderer.stabilizeBackgroundFrame(
        previous.shaderResource.Get(),
        current.shaderResource.Get(),
        filtered.view.Get());
    const std::vector<ReadbackPixel> changed = readback(
        graphics.context.Get(),
        filtered.texture.Get());
    const std::size_t center = static_cast<std::size_t>(testSize.height / 2U)
        * testSize.width
        + testSize.width / 2U;
    // A real desktop change must still pass through instead of freezing the
    // first captured frame for the lifetime of a visible effect.
    BAFX_CHECK(changed[center].red < 0.98F);
    BAFX_CHECK(changed[center].red > 0.90F - 1.0e-3F);
}

BAFX_TEST(warp_background_transport_does_not_promote_emission_to_alpha)
{
    ComApartment apartment;
    const WarpDevice graphics = createWarpDevice();
    FxGpuRenderer renderer(graphics.device.Get(), graphics.context.Get(), testSize);
    renderer.setBloomSettings(FxBloomSettings{0.0F, 7.0F});

    const RenderTarget background = createRenderTarget(graphics.device.Get());
    constexpr std::array<float, 4> lightBackground{
        0.96F,
        0.96F,
        0.96F,
        1.0F};
    graphics.context->ClearRenderTargetView(
        background.view.Get(),
        lightBackground.data());

    const auto render = [&](const float artisticIntensity)
    {
        const RenderTarget target = createRenderTarget(graphics.device.Get());
        renderer.render(
            makeTriangleTransportSnapshot(0.12F, artisticIntensity),
            target.view.Get(),
            BackgroundRenderInput{background.shaderResource.Get()});
        return readback(graphics.context.Get(), target.texture.Get());
    };

    const std::vector<ReadbackPixel> dimEmission = render(0.5F);
    const std::vector<ReadbackPixel> brightEmission = render(12.0F);
    checkValidDesktopPremultiplied(dimEmission, false, true);
    checkValidDesktopPremultiplied(brightEmission, false, true);

    constexpr std::uint32_t triangleLeft = 96U;
    constexpr std::uint32_t triangleTop = 32U;
    constexpr std::uint32_t triangleRight = 160U;
    constexpr std::uint32_t triangleBottom = 96U;
    const float alphaDelta = maximumAlphaDeltaInBox(
        dimEmission,
        brightEmission,
        triangleLeft,
        triangleTop,
        triangleRight,
        triangleBottom);
    const auto directEnergy = [&](const float artisticIntensity)
    {
        const RenderTarget target = createRenderTarget(graphics.device.Get());
        const bafx::fx::FrameSnapshot snapshot =
            makeTriangleTransportSnapshot(0.12F, artisticIntensity);
        const FxGpuFrameCapture capture = renderer.renderAndCapture(
            snapshot,
            target.view.Get());
        return maximumRgbInBox(
            toFloatPixels(capture.directSurface),
            triangleLeft,
            triangleTop,
            triangleRight,
            triangleBottom);
    };
    const float dimEnergy = directEnergy(0.5F);
    const float brightEnergy = directEnergy(12.0F);
    const float dimVisibleEnergy = maximumRgbInBox(
        dimEmission,
        triangleLeft,
        triangleTop,
        triangleRight,
        triangleBottom);
    const float brightVisibleEnergy = maximumRgbInBox(
        brightEmission,
        triangleLeft,
        triangleTop,
        triangleRight,
        triangleBottom);
    BAFX_CHECK(alphaDelta <= 2.0e-3F);
    BAFX_CHECK(brightEnergy - dimEnergy > 0.05F);
    BAFX_CHECK(brightVisibleEnergy - dimVisibleEnergy > 0.05F);
}

BAFX_TEST(warp_background_path_keeps_triangle_alpha_independent_from_the_disk)
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

    const auto render = [&](const float triangleAlpha)
    {
        const RenderTarget target = createRenderTarget(graphics.device.Get());
        renderer.render(
            makeSeparatedDiskAndTriangleSnapshot(triangleAlpha),
            target.view.Get(),
            BackgroundRenderInput{background.shaderResource.Get()});
        return readback(graphics.context.Get(), target.texture.Get());
    };

    const std::vector<ReadbackPixel> brightTriangle = render(0.9F);
    const std::vector<ReadbackPixel> dimTriangle = render(0.1F);

    checkValidDesktopPremultiplied(brightTriangle, false, true);
    checkValidDesktopPremultiplied(dimTriangle, false, true);

    constexpr std::uint32_t diskLeft = 96U;
    constexpr std::uint32_t diskTop = 144U;
    constexpr std::uint32_t diskRight = 160U;
    constexpr std::uint32_t diskBottom = 208U;
    constexpr std::uint32_t triangleLeft = 96U;
    constexpr std::uint32_t triangleTop = 32U;
    constexpr std::uint32_t triangleRight = 160U;
    constexpr std::uint32_t triangleBottom = 96U;

    // Unity Cross2 and Tri2 own independent particle Alpha. The fixed
    // background path must not make Cross2 inherit Tri2's pulse.
    BAFX_CHECK(maximumCompositeRgbDeltaInBox(
        brightTriangle,
        dimTriangle,
        backgroundColor,
        diskLeft,
        diskTop,
        diskRight,
        diskBottom) <= 1.0e-3F);
    BAFX_CHECK(maximumCompositeRgbDeltaInBox(
        brightTriangle,
        dimTriangle,
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
    BAFX_CHECK(capture.bloomResult.width == testSize.width);
    BAFX_CHECK(capture.bloomResult.height == testSize.height);
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
    const std::vector<ReadbackPixel> bloom = toFloatPixels(capture.bloomResult);
    const std::vector<ReadbackPixel> final = toFloatPixels(capture.finalOverlay);
    checkFiniteAndNonNegative(direct);
    checkFiniteAndNonNegative(seed);
    checkFiniteAndNonNegative(bloom);
    checkFiniteAndNonNegative(final);
    const std::size_t center = static_cast<std::size_t>(testSize.height / 2U)
        * testSize.width
        + testSize.width / 2U;
    BAFX_CHECK(direct[center].blue > 1.0F);
    BAFX_CHECK(seed[center].blue > 1.0F);
    BAFX_CHECK(bloom[center].blue > 0.0F);
    BAFX_CHECK(final[center].blue >= direct[center].blue);
}

BAFX_TEST(warp_capture_preserves_directional_alpha_layer_contract)
{
    ComApartment apartment;
    const WarpDevice graphics = createWarpDevice();
    FxGpuRenderer renderer(graphics.device.Get(), graphics.context.Get(), testSize);
    const RenderTarget target = createRenderTarget(graphics.device.Get());

    // Keep Cross2 as an explicit non-Bloom control while Tri2 exercises the
    // Unity additive material's Bloom path.
    bafx::fx::FrameSnapshot snapshot = makeDiskSnapshot(false);
    bafx::fx::Sprite triangle = makeTriangleSnapshot().sprites.front();
    triangle.centerPixels = bafx::fx::PointF{
        static_cast<float>(testSize.width) * 0.25F,
        static_cast<float>(testSize.height) * 0.25F};
    snapshot.sprites.push_back(triangle);

    const FxGpuFrameCapture capture = renderer.renderAndCapture(
        snapshot,
        target.view.Get());
    const std::vector<ReadbackPixel> direct = toFloatPixels(capture.directSurface);
    const std::vector<ReadbackPixel> seed = toFloatPixels(capture.bloomSeed);
    const std::vector<ReadbackPixel> final = toFloatPixels(capture.finalOverlay);

    constexpr float fp16Tolerance = 2.0e-3F;
    bool foundBloomExpandedAlpha = false;
    bool foundNonBloomDirectAlpha = false;
    for (std::size_t index = 0U; index < direct.size(); ++index)
    {
        BAFX_CHECK(seed[index].alpha <= direct[index].alpha + fp16Tolerance);
        BAFX_CHECK(direct[index].alpha <= final[index].alpha + fp16Tolerance);
        if (final[index].alpha > direct[index].alpha + fp16Tolerance)
        {
            foundBloomExpandedAlpha = true;
        }
        if (direct[index].alpha > seed[index].alpha + fp16Tolerance)
        {
            foundNonBloomDirectAlpha = true;
        }
    }

    // Exercise both strict inequalities so this cannot regress into an Alpha
    // equality check that rejects valid non-Bloom or propagated-Bloom pixels.
    BAFX_CHECK(foundBloomExpandedAlpha);
    BAFX_CHECK(foundNonBloomDirectAlpha);
}

BAFX_TEST(warp_global_opacity_preserves_dissolve_shape_and_scales_once)
{
    ComApartment apartment;
    const WarpDevice graphics = createWarpDevice();
    FxGpuRenderer renderer(graphics.device.Get(), graphics.context.Get(), testSize);
    const RenderTarget target = createRenderTarget(graphics.device.Get());

    const auto renderDirect = [&](const float opacity)
    {
        bafx::fx::FrameSnapshot snapshot = makeDissolveRingSnapshot();
        snapshot.globalOpacity = opacity;
        return toFloatPixels(
            renderer.renderAndCapture(snapshot, target.view.Get()).directSurface);
    };
    const std::vector<ReadbackPixel> full = renderDirect(1.0F);
    const std::vector<ReadbackPixel> half = renderDirect(0.5F);

    std::size_t comparedPixels = 0U;
    for (std::size_t index = 0U; index < full.size(); ++index)
    {
        const bool fullVisible = full[index].alpha > 1.0e-4F;
        const bool halfVisible = half[index].alpha > 1.0e-4F;
        BAFX_CHECK(fullVisible == halfVisible);
        if (full[index].alpha <= 0.05F)
        {
            continue;
        }

        ++comparedPixels;
        BAFX_CHECK_NEAR(half[index].alpha, full[index].alpha * 0.5F, 2.0e-3F);
        BAFX_CHECK_NEAR(half[index].red, full[index].red * 0.5F, 8.0e-3F);
        BAFX_CHECK_NEAR(half[index].green, full[index].green * 0.5F, 8.0e-3F);
        BAFX_CHECK_NEAR(half[index].blue, full[index].blue * 0.5F, 8.0e-3F);
    }
    BAFX_CHECK(comparedPixels > 100U);
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
    BAFX_CHECK(isZeroImage(disabledBloom.bloomResult));
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

BAFX_TEST(warp_bloom_layer_toggle_bypasses_output_without_stale_bloom)
{
    ComApartment apartment;
    const WarpDevice graphics = createWarpDevice();
    FxGpuRenderer renderer(graphics.device.Get(), graphics.context.Get(), testSize);
    const RenderTarget target = createRenderTarget(graphics.device.Get());

    const FxGpuFrameCapture enabled = renderer.renderAndCapture(
        makeDiskSnapshot(true),
        target.view.Get());
    BAFX_CHECK(!isZeroImage(enabled.bloomResult));

    FxBloomSettings disabledSettings{};
    disabledSettings.enabled = false;
    renderer.setBloomSettings(disabledSettings);
    const FxGpuFrameCapture disabled = renderer.renderAndCapture(
        makeDiskSnapshot(true),
        target.view.Get());
    BAFX_CHECK(!isZeroImage(disabled.directSurface));
    BAFX_CHECK(isZeroImage(disabled.bloomResult));
    BAFX_CHECK(maximumRgbOutsideSprite(toFloatPixels(disabled.finalOverlay))
        <= 1.0e-6F);

    renderer.setBloomSettings(FxBloomSettings{});
    const FxGpuFrameCapture restored = renderer.renderAndCapture(
        makeDiskSnapshot(true),
        target.view.Get());
    BAFX_CHECK(!isZeroImage(restored.bloomResult));
}

BAFX_TEST(active_fx_roi_plan_validation_cache_hits_and_replans_valid_motion)
{
    constexpr bafx::core::RectI monitor{
        0,
        0,
        static_cast<std::int32_t>(testSize.width),
        static_cast<std::int32_t>(testSize.height)};
    const bafx::core::UnityBloomPlan bloom = makeUnityBloomPlanForTest();
    const FxActiveRoi first = makeActiveFxRoi(
        bafx::core::RectI{96, 96, 144, 144});
    const FxActiveRoi moved = makeActiveFxRoi(
        bafx::core::RectI{112, 104, 160, 152});
    bafx::windows::detail::ActiveFxRoiPlanValidationCache cache;

    const auto firstValidation = cache.validate(first.passPlan, monitor, bloom);
    BAFX_CHECK(firstValidation.valid);
    BAFX_CHECK(!firstValidation.cacheHit);
    const auto repeatedFirst = cache.validate(first.passPlan, monitor, bloom);
    BAFX_CHECK(repeatedFirst.valid);
    BAFX_CHECK(repeatedFirst.cacheHit);

    const auto movedValidation = cache.validate(moved.passPlan, monitor, bloom);
    BAFX_CHECK(movedValidation.valid);
    BAFX_CHECK(!movedValidation.cacheHit);
    const auto repeatedMove = cache.validate(moved.passPlan, monitor, bloom);
    BAFX_CHECK(repeatedMove.valid);
    BAFX_CHECK(repeatedMove.cacheHit);

    FxActiveRoi tampered = moved;
    ++tampered.passPlan.totalPixels.candidatePixels;
    const auto rejected = cache.validate(tampered.passPlan, monitor, bloom);
    BAFX_CHECK(!rejected.valid);
    BAFX_CHECK(!rejected.cacheHit);
    const auto validAfterRejection = cache.validate(moved.passPlan, monitor, bloom);
    BAFX_CHECK(validAfterRejection.valid);
    BAFX_CHECK(validAfterRejection.cacheHit);
}

BAFX_TEST(active_fx_roi_plan_validation_cache_invalidates_render_context)
{
    constexpr bafx::core::RectI initialMonitor{
        0,
        0,
        static_cast<std::int32_t>(testSize.width),
        static_cast<std::int32_t>(testSize.height)};
    constexpr WindowSize resized{319U, 181U};
    constexpr bafx::core::RectI resizedMonitor{
        0,
        0,
        static_cast<std::int32_t>(resized.width),
        static_cast<std::int32_t>(resized.height)};
    constexpr FxBloomSettings lowDiffusion{1.0F, 4.0F};
    const bafx::core::UnityBloomPlan initialBloom = makeUnityBloomPlanForTest();
    const bafx::core::UnityBloomPlan resizedBloom = makeUnityBloomPlanForTest(
        FxBloomSettings{},
        resized);
    const bafx::core::UnityBloomPlan lowDiffusionBloom =
        makeUnityBloomPlanForTest(lowDiffusion, resized);
    const FxActiveRoi initial = makeActiveFxRoi(
        bafx::core::RectI{96, 96, 144, 144});
    const FxActiveRoi resizedRoi = makeActiveFxRoi(
        bafx::core::RectI{130, 70, 178, 118},
        FxBloomSettings{},
        resized);
    const FxActiveRoi lowDiffusionRoi = makeActiveFxRoi(
        bafx::core::RectI{130, 70, 178, 118},
        lowDiffusion,
        resized);
    bafx::windows::detail::ActiveFxRoiPlanValidationCache cache;

    BAFX_CHECK(cache.validate(initial.passPlan, initialMonitor, initialBloom).valid);
    BAFX_CHECK(
        cache.validate(initial.passPlan, initialMonitor, initialBloom).cacheHit);

    const auto staleAfterResize = cache.validate(
        initial.passPlan,
        resizedMonitor,
        resizedBloom);
    BAFX_CHECK(!staleAfterResize.valid);
    BAFX_CHECK(!staleAfterResize.cacheHit);
    const auto afterResize = cache.validate(
        resizedRoi.passPlan,
        resizedMonitor,
        resizedBloom);
    BAFX_CHECK(afterResize.valid);
    BAFX_CHECK(!afterResize.cacheHit);
    BAFX_CHECK(
        cache.validate(resizedRoi.passPlan, resizedMonitor, resizedBloom).cacheHit);

    const auto staleAfterDiffusion = cache.validate(
        resizedRoi.passPlan,
        resizedMonitor,
        lowDiffusionBloom);
    BAFX_CHECK(!staleAfterDiffusion.valid);
    BAFX_CHECK(!staleAfterDiffusion.cacheHit);
    const auto afterDiffusion = cache.validate(
        lowDiffusionRoi.passPlan,
        resizedMonitor,
        lowDiffusionBloom);
    BAFX_CHECK(afterDiffusion.valid);
    BAFX_CHECK(!afterDiffusion.cacheHit);
    BAFX_CHECK(
        cache.validate(
            lowDiffusionRoi.passPlan,
            resizedMonitor,
            lowDiffusionBloom).cacheHit);

    cache.reset();
    const auto afterReset = cache.validate(
        lowDiffusionRoi.passPlan,
        resizedMonitor,
        lowDiffusionBloom);
    BAFX_CHECK(afterReset.valid);
    BAFX_CHECK(!afterReset.cacheHit);
}

BAFX_TEST(warp_active_fx_roi_cached_validation_tampering_falls_back_exactly)
{
    ComApartment apartment;
    const WarpDevice graphics = createWarpDevice();
    const bafx::fx::FrameSnapshot snapshot = makeDiskSnapshot(true);
    const FxActiveRoi validRoi = makeActiveFxRoi(snapshot);

    FxGpuRenderer referenceRenderer(
        graphics.device.Get(),
        graphics.context.Get(),
        testSize);
    const RenderTarget referenceTarget = createRenderTarget(graphics.device.Get());
    referenceRenderer.render(snapshot, referenceTarget.view.Get());
    const Rgba16FloatImage reference = readbackRgba16FloatTexture(
        graphics.context.Get(),
        referenceTarget.texture.Get());

    FxGpuRenderer cachedRenderer(
        graphics.device.Get(),
        graphics.context.Get(),
        testSize);
    const RenderTarget warmupTarget = createRenderTarget(graphics.device.Get());
    const RenderTarget steadyTarget = createRenderTarget(graphics.device.Get());
    BAFX_CHECK(
        cachedRenderer.render(
            snapshot,
            warmupTarget.view.Get(),
            std::nullopt,
            nullptr,
            nullptr,
            validRoi).primaryActiveFxRoi.warmup);
    BAFX_CHECK(
        cachedRenderer.render(
            snapshot,
            steadyTarget.view.Get(),
            std::nullopt,
            nullptr,
            nullptr,
            validRoi).primaryActiveFxRoi.actualPath
        == FxActiveRoiActualPath::RoiPyramid);

    std::array<FxActiveRoi, 6U> tampered{};
    tampered.fill(validRoi);
    ++tampered[0U].passPlan.basePlan.guardX;
    ++tampered[1U].passPlan.downRects[0U].left;
    --tampered[2U].passPlan.upRects[0U].right;
    ++tampered[3U].passPlan.resolveRect.left;
    ++tampered[4U].passPlan.totalPixels.candidatePixels;
    ++tampered[5U].passPlan.mipCount;

    for (const FxActiveRoi& invalidRoi : tampered)
    {
        const RenderTarget fallbackTarget = createRenderTarget(
            graphics.device.Get());
        const FxRenderCpuDiagnostics diagnostics = cachedRenderer.render(
            snapshot,
            fallbackTarget.view.Get(),
            std::nullopt,
            nullptr,
            nullptr,
            invalidRoi);
        BAFX_CHECK(!diagnostics.activeFxRoiApplied);
        BAFX_CHECK(diagnostics.primaryActiveFxRoi.requested);
        BAFX_CHECK(!diagnostics.primaryActiveFxRoi.eligible);
        BAFX_CHECK(
            diagnostics.primaryActiveFxRoi.actualPath
            == FxActiveRoiActualPath::FullScreen);
        BAFX_CHECK(
            diagnostics.primaryActiveFxRoi.decisionReason
            == FxActiveRoiDecisionReason::RendererFallback);
        checkRgba16BitExactAndFinite(
            reference,
            readbackRgba16FloatTexture(
                graphics.context.Get(),
                fallbackTarget.texture.Get()));
    }
}

BAFX_TEST(warp_active_fx_roi_pyramid_matches_full_screen_pixels)
{
    ComApartment apartment;
    const WarpDevice graphics = createWarpDevice();
    FxGpuRenderer renderer(graphics.device.Get(), graphics.context.Get(), testSize);
    const bafx::fx::FrameSnapshot snapshot = makeDiskSnapshot(true);

    const RenderTarget fullTarget = createRenderTarget(graphics.device.Get());
    const FxRenderCpuDiagnostics fullDiagnostics = renderer.render(
        snapshot,
        fullTarget.view.Get());
    BAFX_CHECK(!fullDiagnostics.activeFxRoiApplied);
    BAFX_CHECK(
        fullDiagnostics.primaryActiveFxRoi.actualPath
        == FxActiveRoiActualPath::FullScreen);
    BAFX_CHECK(
        fullDiagnostics.primaryActiveFxRoi.drawnPixels
        == fullDiagnostics.primaryActiveFxRoi.fullPixels);
    BAFX_CHECK(
        fullDiagnostics.primaryActiveFxRoi.stages.resolve.drawnPixels
        == static_cast<std::uint64_t>(testSize.width) * testSize.height);

    const FxActiveRoi roiRequest = makeActiveFxRoi(snapshot);
    const RenderTarget warmupTarget = createRenderTarget(graphics.device.Get());
    const FxRenderCpuDiagnostics warmupDiagnostics = renderer.render(
        snapshot,
        warmupTarget.view.Get(),
        std::nullopt,
        nullptr,
        nullptr,
        roiRequest);
    BAFX_CHECK(warmupDiagnostics.activeFxRoiApplied);
    BAFX_CHECK(
        warmupDiagnostics.activeFxRoiPixels
        == warmupDiagnostics.primaryActiveFxRoi.drawnPixels);
    BAFX_CHECK(warmupDiagnostics.primaryActiveFxRoi.requested);
    BAFX_CHECK(warmupDiagnostics.primaryActiveFxRoi.eligible);
    BAFX_CHECK(warmupDiagnostics.primaryActiveFxRoi.executed);
    BAFX_CHECK(warmupDiagnostics.primaryActiveFxRoi.warmup);
    BAFX_CHECK(
        warmupDiagnostics.primaryActiveFxRoi.actualPath
        == FxActiveRoiActualPath::RoiWarmup);
    BAFX_CHECK(
        warmupDiagnostics.primaryActiveFxRoi.decisionReason
        == FxActiveRoiDecisionReason::Applied);
    BAFX_CHECK(
        warmupDiagnostics.primaryActiveFxRoi.stages.prefilter.clearedPixels
        == warmupDiagnostics.primaryActiveFxRoi.stages.prefilter.fullPixels);
    BAFX_CHECK(
        warmupDiagnostics.primaryActiveFxRoi.stages.downsample.clearedPixels
        == warmupDiagnostics.primaryActiveFxRoi.stages.downsample.fullPixels);
    BAFX_CHECK(
        warmupDiagnostics.primaryActiveFxRoi.stages.upsample.clearedPixels
        == warmupDiagnostics.primaryActiveFxRoi.stages.upsample.fullPixels);
    BAFX_CHECK(
        warmupDiagnostics.primaryActiveFxRoi.stages.resolve.clearedPixels
        == warmupDiagnostics.primaryActiveFxRoi.stages.resolve.fullPixels);

    const RenderTarget steadyTarget = createRenderTarget(graphics.device.Get());
    const FxRenderCpuDiagnostics steadyDiagnostics = renderer.render(
        snapshot,
        steadyTarget.view.Get(),
        std::nullopt,
        nullptr,
        nullptr,
        roiRequest);
    BAFX_CHECK(steadyDiagnostics.activeFxRoiApplied);
    BAFX_CHECK(!steadyDiagnostics.primaryActiveFxRoi.warmup);
    BAFX_CHECK(
        steadyDiagnostics.primaryActiveFxRoi.actualPath
        == FxActiveRoiActualPath::RoiPyramid);
    BAFX_CHECK(
        steadyDiagnostics.primaryActiveFxRoi.stages.prefilter.drawnPixels
        == steadyDiagnostics.primaryActiveFxRoi.stages.prefilter.candidatePixels);
    BAFX_CHECK(
        steadyDiagnostics.primaryActiveFxRoi.stages.downsample.drawnPixels
        == steadyDiagnostics.primaryActiveFxRoi.stages.downsample.candidatePixels);
    BAFX_CHECK(
        steadyDiagnostics.primaryActiveFxRoi.stages.upsample.drawnPixels
        == steadyDiagnostics.primaryActiveFxRoi.stages.upsample.candidatePixels);
    BAFX_CHECK(
        steadyDiagnostics.primaryActiveFxRoi.stages.resolve.drawnPixels
        == steadyDiagnostics.primaryActiveFxRoi.stages.resolve.candidatePixels);
    BAFX_CHECK(
        steadyDiagnostics.primaryActiveFxRoi.stages.prefilter.clearedPixels
        == 0U);
    BAFX_CHECK(
        steadyDiagnostics.primaryActiveFxRoi.stages.downsample.clearedPixels
        == 0U);
    BAFX_CHECK(
        steadyDiagnostics.primaryActiveFxRoi.stages.upsample.clearedPixels
        == 0U);
    BAFX_CHECK(
        steadyDiagnostics.primaryActiveFxRoi.stages.resolve.clearedPixels
        == steadyDiagnostics.primaryActiveFxRoi.stages.resolve.fullPixels);
    BAFX_CHECK(
        steadyDiagnostics.primaryActiveFxRoi.clearedPixels
        == steadyDiagnostics.primaryActiveFxRoi.stages.resolve.fullPixels);

    checkRgba16BitExactAndFinite(
        readbackRgba16FloatTexture(
            graphics.context.Get(),
            fullTarget.texture.Get()),
        readbackRgba16FloatTexture(
            graphics.context.Get(),
            steadyTarget.texture.Get()));

    renderer.resetActiveFxRoiState();
    const RenderTarget resetTarget = createRenderTarget(graphics.device.Get());
    const FxRenderCpuDiagnostics resetDiagnostics = renderer.render(
        snapshot,
        resetTarget.view.Get(),
        std::nullopt,
        nullptr,
        nullptr,
        roiRequest);
    BAFX_CHECK(resetDiagnostics.primaryActiveFxRoi.warmup);
    BAFX_CHECK(
        resetDiagnostics.primaryActiveFxRoi.stages.prefilter.clearedPixels
        == resetDiagnostics.primaryActiveFxRoi.stages.prefilter.fullPixels);

    const RenderTarget areaTarget = createRenderTarget(graphics.device.Get());
    const FxRenderCpuDiagnostics areaDiagnostics = renderer.render(
        snapshot,
        areaTarget.view.Get(),
        std::nullopt,
        nullptr,
        nullptr,
        makeActiveFxRoi(bafx::core::RectI{0, 0, 256, 256}));
    BAFX_CHECK(!areaDiagnostics.activeFxRoiApplied);
    BAFX_CHECK(
        areaDiagnostics.primaryActiveFxRoi.decisionReason
        == FxActiveRoiDecisionReason::AreaTooLarge);

    FxActiveRoi invalidRoi = roiRequest;
    ++invalidRoi.passPlan.mipCount;
    const RenderTarget invalidTarget = createRenderTarget(graphics.device.Get());
    const FxRenderCpuDiagnostics invalidDiagnostics = renderer.render(
        snapshot,
        invalidTarget.view.Get(),
        std::nullopt,
        nullptr,
        nullptr,
        invalidRoi);
    BAFX_CHECK(!invalidDiagnostics.activeFxRoiApplied);
    BAFX_CHECK(invalidDiagnostics.primaryActiveFxRoi.requested);
    BAFX_CHECK(!invalidDiagnostics.primaryActiveFxRoi.eligible);
    BAFX_CHECK(
        invalidDiagnostics.primaryActiveFxRoi.actualPath
        == FxActiveRoiActualPath::FullScreen);
    BAFX_CHECK(
        invalidDiagnostics.primaryActiveFxRoi.decisionReason
        == FxActiveRoiDecisionReason::RendererFallback);

    const RenderTarget idleTarget = createRenderTarget(graphics.device.Get());
    const FxRenderCpuDiagnostics idleDiagnostics = renderer.render(
        bafx::fx::FrameSnapshot{},
        idleTarget.view.Get());
    BAFX_CHECK(
        idleDiagnostics.primaryActiveFxRoi.actualPath
        == FxActiveRoiActualPath::Idle);
    const RenderTarget restartedTarget = createRenderTarget(graphics.device.Get());
    const FxRenderCpuDiagnostics restartedDiagnostics = renderer.render(
        snapshot,
        restartedTarget.view.Get(),
        std::nullopt,
        nullptr,
        nullptr,
        roiRequest);
    BAFX_CHECK(restartedDiagnostics.primaryActiveFxRoi.warmup);
    BAFX_CHECK(
        restartedDiagnostics.primaryActiveFxRoi.stages.prefilter.clearedPixels
        == restartedDiagnostics.primaryActiveFxRoi.stages.prefilter.fullPixels);
}

BAFX_TEST(warp_active_fx_roi_click_and_trail_layers_are_fp16_exact)
{
    struct ContentCase final
    {
        bafx::fx::FrameSnapshot snapshot{};
        FxBloomSettings bloom{};
    };

    const std::array<ContentCase, 2U> cases{
        ContentCase{makeDiskSnapshot(true), FxBloomSettings{}},
        ContentCase{
            makeCompactTrailSnapshot(),
            FxBloomSettings{1.0F, 4.0F}}};

    ComApartment apartment;
    const WarpDevice graphics = createWarpDevice();
    for (const ContentCase& testCase : cases)
    {
        const FxActiveRoi roi = makeActiveFxRoi(
            testCase.snapshot,
            testCase.bloom);
        FxGpuRenderer referenceRenderer(
            graphics.device.Get(),
            graphics.context.Get(),
            testSize,
            testCase.bloom);
        FxGpuRenderer roiRenderer(
            graphics.device.Get(),
            graphics.context.Get(),
            testSize,
            testCase.bloom);
        const RenderTarget referenceTarget = createRenderTarget(
            graphics.device.Get());
        const RenderTarget roiTarget = createRenderTarget(graphics.device.Get());

        const FxRenderCpuDiagnostics warmup = roiRenderer.render(
            testCase.snapshot,
            roiTarget.view.Get(),
            std::nullopt,
            nullptr,
            nullptr,
            roi);
        BAFX_CHECK(
            warmup.primaryActiveFxRoi.actualPath
            == FxActiveRoiActualPath::RoiWarmup);
        const FxRenderCpuDiagnostics steady = roiRenderer.render(
            testCase.snapshot,
            roiTarget.view.Get(),
            std::nullopt,
            nullptr,
            nullptr,
            roi);
        BAFX_CHECK(
            steady.primaryActiveFxRoi.actualPath
            == FxActiveRoiActualPath::RoiPyramid);
        BAFX_CHECK(
            steady.primaryActiveFxRoi.stages.downsample.drawnPixels
            == steady.primaryActiveFxRoi.stages.downsample.candidatePixels);
        BAFX_CHECK(
            steady.primaryActiveFxRoi.stages.upsample.drawnPixels
            == steady.primaryActiveFxRoi.stages.upsample.candidatePixels);

        const FxGpuFrameCapture reference = referenceRenderer.renderAndCapture(
            testCase.snapshot,
            referenceTarget.view.Get());
        const FxGpuFrameCapture candidate = roiRenderer.renderAndCapture(
            testCase.snapshot,
            roiTarget.view.Get(),
            roi);
        BAFX_CHECK(reference.intermediateLayersValid);
        BAFX_CHECK(candidate.intermediateLayersValid);
        BAFX_CHECK(!reference.bloomDown.empty());
        if (testCase.bloom.diffusion > 4.0F)
        {
            BAFX_CHECK(!reference.bloomUp.empty());
        }
        checkCaptureBitExactAndFinite(reference, candidate);
    }
}

BAFX_TEST(warp_active_fx_roi_resize_rewarms_odd_target_and_stays_fp16_exact)
{
    constexpr FxBloomSettings bloom{1.0F, 6.0F};
    constexpr WindowSize resized{319U, 181U};
    const bafx::fx::FrameSnapshot initialSnapshot = makeDiskSnapshotForSize(
        testSize,
        true);
    const bafx::fx::FrameSnapshot resizedSnapshot = makeDiskSnapshotForSize(
        resized,
        true);

    ComApartment apartment;
    const WarpDevice graphics = createWarpDevice();
    FxGpuRenderer referenceRenderer(
        graphics.device.Get(),
        graphics.context.Get(),
        testSize,
        bloom);
    FxGpuRenderer roiRenderer(
        graphics.device.Get(),
        graphics.context.Get(),
        testSize,
        bloom);
    const RenderTarget initialTarget = createRenderTarget(graphics.device.Get());
    const FxRenderCpuDiagnostics initial = roiRenderer.render(
        initialSnapshot,
        initialTarget.view.Get(),
        std::nullopt,
        nullptr,
        nullptr,
        makeActiveFxRoi(initialSnapshot, bloom));
    BAFX_CHECK(
        initial.primaryActiveFxRoi.actualPath
        == FxActiveRoiActualPath::RoiWarmup);

    referenceRenderer.resize(resized);
    roiRenderer.resize(resized);
    const FxActiveRoi resizedRoi = makeActiveFxRoi(
        resizedSnapshot,
        bloom,
        resized);
    const RenderTarget referenceTarget = createRenderTarget(
        graphics.device.Get(),
        resized);
    const RenderTarget roiTarget = createRenderTarget(
        graphics.device.Get(),
        resized);

    const FxRenderCpuDiagnostics resizedWarmup = roiRenderer.render(
        resizedSnapshot,
        roiTarget.view.Get(),
        std::nullopt,
        nullptr,
        nullptr,
        resizedRoi);
    BAFX_CHECK(resizedWarmup.primaryActiveFxRoi.warmup);
    BAFX_CHECK(
        resizedWarmup.primaryActiveFxRoi.actualPath
        == FxActiveRoiActualPath::RoiWarmup);
    BAFX_CHECK(
        resizedWarmup.primaryActiveFxRoi.stages.prefilter.clearedPixels
        == resizedWarmup.primaryActiveFxRoi.stages.prefilter.fullPixels);

    const FxRenderCpuDiagnostics resizedSteady = roiRenderer.render(
        resizedSnapshot,
        roiTarget.view.Get(),
        std::nullopt,
        nullptr,
        nullptr,
        resizedRoi);
    BAFX_CHECK(
        resizedSteady.primaryActiveFxRoi.actualPath
        == FxActiveRoiActualPath::RoiPyramid);
    const FxGpuFrameCapture reference = referenceRenderer.renderAndCapture(
        resizedSnapshot,
        referenceTarget.view.Get());
    const FxGpuFrameCapture candidate = roiRenderer.renderAndCapture(
        resizedSnapshot,
        roiTarget.view.Get(),
        resizedRoi);
    BAFX_CHECK(reference.finalOverlay.width == resized.width);
    BAFX_CHECK(reference.finalOverlay.height == resized.height);
    checkCaptureBitExactAndFinite(reference, candidate);
}

BAFX_TEST(warp_active_fx_roi_spout_fx_only_is_exact_in_warmup_and_steady_state)
{
    ComApartment apartment;
    const WarpDevice graphics = createWarpDevice();
    constexpr FxBloomSettings fourTapBloom{1.0F, 4.0F};
    bafx::fx::FrameSnapshot snapshot = makeDiskSnapshot(true);
    snapshot.trailStrokes = makeCompactTrailSnapshot().trailStrokes;
    const FxActiveRoi roi = makeActiveFxRoi(snapshot, fourTapBloom);

    FxGpuRenderer referenceRenderer(
        graphics.device.Get(),
        graphics.context.Get(),
        testSize,
        fourTapBloom);
    const RenderTarget referenceDesktop = createRenderTarget(
        graphics.device.Get());
    const RenderTarget referenceRecording = createRecordingRenderTarget(
        graphics.device.Get());
    referenceRenderer.render(
        snapshot,
        referenceDesktop.view.Get(),
        std::nullopt,
        nullptr,
        referenceRecording.view.Get());
    const Rgba16FloatImage expectedDesktop = readbackRgba16FloatTexture(
        graphics.context.Get(),
        referenceDesktop.texture.Get());
    const Bgra8Image expectedRecording = readbackBgra8(
        graphics.context.Get(),
        referenceRecording.texture.Get());

    FxGpuRenderer roiRenderer(
        graphics.device.Get(),
        graphics.context.Get(),
        testSize,
        fourTapBloom);
    const RenderTarget warmupDesktop = createRenderTarget(graphics.device.Get());
    const RenderTarget warmupRecording = createRecordingRenderTarget(
        graphics.device.Get());
    const FxRenderCpuDiagnostics warmup = roiRenderer.render(
        snapshot,
        warmupDesktop.view.Get(),
        std::nullopt,
        nullptr,
        warmupRecording.view.Get(),
        roi);
    BAFX_CHECK(
        warmup.primaryActiveFxRoi.actualPath
        == FxActiveRoiActualPath::RoiWarmup);
    BAFX_CHECK(!warmup.recordingRebuildActiveFxRoi.executed);
    checkRgba16BitExactAndFinite(
        expectedDesktop,
        readbackRgba16FloatTexture(
            graphics.context.Get(),
            warmupDesktop.texture.Get()));
    BAFX_CHECK(sameBgra8(
        expectedRecording,
        readbackBgra8(
            graphics.context.Get(),
            warmupRecording.texture.Get())));

    const RenderTarget steadyDesktop = createRenderTarget(graphics.device.Get());
    const RenderTarget steadyRecording = createRecordingRenderTarget(
        graphics.device.Get());
    const FxRenderCpuDiagnostics steady = roiRenderer.render(
        snapshot,
        steadyDesktop.view.Get(),
        std::nullopt,
        nullptr,
        steadyRecording.view.Get(),
        roi);
    BAFX_CHECK(
        steady.primaryActiveFxRoi.actualPath
        == FxActiveRoiActualPath::RoiPyramid);
    BAFX_CHECK(!steady.recordingRebuildActiveFxRoi.executed);
    checkRgba16BitExactAndFinite(
        expectedDesktop,
        readbackRgba16FloatTexture(
            graphics.context.Get(),
            steadyDesktop.texture.Get()));
    BAFX_CHECK(sameBgra8(
        expectedRecording,
        readbackBgra8(
            graphics.context.Get(),
            steadyRecording.texture.Get())));
}

BAFX_TEST(warp_active_fx_roi_context1_unavailable_falls_back_exactly)
{
    checkActiveFxRoiUnavailableFallback(FxGpuRendererFeaturePolicy{
        .allowActiveFxRoiClearView = false});
}

BAFX_TEST(warp_active_fx_roi_clearview_capability_false_falls_back_exactly)
{
    checkActiveFxRoiUnavailableFallback(FxGpuRendererFeaturePolicy{
        .allowActiveFxRoiClearView = true,
        .activeFxRoiClearViewCapabilityOverride = false});
}

BAFX_TEST(warp_recording_roi_reports_shared_target_full_write_warmup)
{
    ComApartment apartment;
    const WarpDevice graphics = createWarpDevice();
    FxGpuRenderer renderer(graphics.device.Get(), graphics.context.Get(), testSize);
    const RenderTarget desktopTarget = createRenderTarget(graphics.device.Get());
    const RenderTarget recordingTarget = createRecordingRenderTarget(
        graphics.device.Get());
    const RenderTarget backgroundTarget = createRenderTarget(graphics.device.Get());
    constexpr std::array<float, 4> background{0.25F, 0.5F, 0.75F, 1.0F};
    graphics.context->ClearRenderTargetView(
        backgroundTarget.view.Get(),
        background.data());

    const FxRenderCpuDiagnostics diagnostics = renderer.render(
        makeDiskSnapshot(true),
        desktopTarget.view.Get(),
        BackgroundRenderInput{backgroundTarget.shaderResource.Get()},
        nullptr,
        recordingTarget.view.Get(),
        makeActiveFxRoi(makeDiskSnapshot(true)));

    BAFX_CHECK(
        diagnostics.primaryActiveFxRoi.actualPath
        == FxActiveRoiActualPath::FullScreen);
    BAFX_CHECK(
        diagnostics.primaryActiveFxRoi.decisionReason
        == FxActiveRoiDecisionReason::BackgroundDifferentialBloom);
    BAFX_CHECK(diagnostics.recordingRebuildActiveFxRoi.requested);
    BAFX_CHECK(diagnostics.recordingRebuildActiveFxRoi.eligible);
    BAFX_CHECK(diagnostics.recordingRebuildActiveFxRoi.warmup);
    BAFX_CHECK(
        diagnostics.recordingRebuildActiveFxRoi.actualPath
        == FxActiveRoiActualPath::RoiWarmup);
    BAFX_CHECK(
        diagnostics.recordingRebuildActiveFxRoi.decisionReason
        == FxActiveRoiDecisionReason::SharedTargetFullWrite);
    BAFX_CHECK(
        diagnostics.recordingRebuildActiveFxRoi.stages.prefilter.drawnPixels
        == diagnostics.recordingRebuildActiveFxRoi.stages.prefilter.candidatePixels);
    BAFX_CHECK(
        diagnostics.recordingRebuildActiveFxRoi.stages.prefilter.clearedPixels
        == diagnostics.recordingRebuildActiveFxRoi.stages.prefilter.fullPixels);
}

BAFX_TEST(warp_active_fx_roi_clears_previous_non_overlapping_motion)
{
    ComApartment apartment;
    const WarpDevice graphics = createWarpDevice();
    FxGpuRenderer renderer(graphics.device.Get(), graphics.context.Get(), testSize);
    bafx::fx::FrameSnapshot leftSnapshot = makeDiskSnapshot(true);
    leftSnapshot.sprites.front().centerPixels.x = 48.0F;
    bafx::fx::FrameSnapshot rightSnapshot = makeDiskSnapshot(true);
    rightSnapshot.sprites.front().centerPixels.x = 208.0F;

    const RenderTarget leftTarget = createRenderTarget(graphics.device.Get());
    const FxRenderCpuDiagnostics leftDiagnostics = renderer.render(
        leftSnapshot,
        leftTarget.view.Get(),
        std::nullopt,
        nullptr,
        nullptr,
        makeActiveFxRoi(leftSnapshot));
    BAFX_CHECK(leftDiagnostics.primaryActiveFxRoi.warmup);

    const RenderTarget movedTarget = createRenderTarget(graphics.device.Get());
    const FxRenderCpuDiagnostics movedDiagnostics = renderer.render(
        rightSnapshot,
        movedTarget.view.Get(),
        std::nullopt,
        nullptr,
        nullptr,
        makeActiveFxRoi(rightSnapshot));
    BAFX_CHECK(
        movedDiagnostics.primaryActiveFxRoi.actualPath
        == FxActiveRoiActualPath::RoiPyramid);
    BAFX_CHECK(
        movedDiagnostics.primaryActiveFxRoi.stages.prefilter.drawnPixels
        == movedDiagnostics.primaryActiveFxRoi.stages.prefilter.candidatePixels);
    BAFX_CHECK(
        movedDiagnostics.primaryActiveFxRoi.stages.prefilter.clearedPixels
        == leftDiagnostics.primaryActiveFxRoi.stages.prefilter.candidatePixels);
    BAFX_CHECK(movedDiagnostics.primaryActiveFxRoi.clearedPixels > 0U);

    FxGpuRenderer referenceRenderer(
        graphics.device.Get(),
        graphics.context.Get(),
        testSize);
    const RenderTarget referenceTarget = createRenderTarget(graphics.device.Get());
    referenceRenderer.render(rightSnapshot, referenceTarget.view.Get());
    checkRgba16BitExactAndFinite(
        readbackRgba16FloatTexture(
            graphics.context.Get(),
            referenceTarget.texture.Get()),
        readbackRgba16FloatTexture(
            graphics.context.Get(),
            movedTarget.texture.Get()));

    const FxGpuFrameCapture referenceCapture = referenceRenderer.renderAndCapture(
        leftSnapshot,
        referenceTarget.view.Get());
    const FxGpuFrameCapture returnedCapture = renderer.renderAndCapture(
        leftSnapshot,
        movedTarget.view.Get(),
        makeActiveFxRoi(leftSnapshot));
    checkCaptureBitExactAndFinite(referenceCapture, returnedCapture);
}

BAFX_TEST(warp_active_fx_roi_transitions_and_empty_restart_stay_exact)
{
    ComApartment apartment;
    const WarpDevice graphics = createWarpDevice();
    constexpr FxBloomSettings fourTapBloom{1.0F, 4.0F};
    const bafx::fx::FrameSnapshot snapshot = makeDiskSnapshot(true);
    const FxActiveRoi roi = makeActiveFxRoi(snapshot, fourTapBloom);

    FxGpuRenderer referenceRenderer(
        graphics.device.Get(),
        graphics.context.Get(),
        testSize,
        fourTapBloom);
    const RenderTarget referenceTarget = createRenderTarget(
        graphics.device.Get());
    referenceRenderer.render(snapshot, referenceTarget.view.Get());
    const Rgba16FloatImage reference = readbackRgba16FloatTexture(
        graphics.context.Get(),
        referenceTarget.texture.Get());

    FxGpuRenderer renderer(
        graphics.device.Get(),
        graphics.context.Get(),
        testSize,
        fourTapBloom);
    const RenderTarget target = createRenderTarget(graphics.device.Get());

    const FxRenderCpuDiagnostics firstRoi = renderer.render(
        snapshot,
        target.view.Get(),
        std::nullopt,
        nullptr,
        nullptr,
        roi);
    BAFX_CHECK(firstRoi.primaryActiveFxRoi.warmup);
    BAFX_CHECK(
        firstRoi.primaryActiveFxRoi.actualPath
        == FxActiveRoiActualPath::RoiWarmup);
    BAFX_CHECK(
        firstRoi.primaryActiveFxRoi.stages.prefilter.clearedPixels
        == firstRoi.primaryActiveFxRoi.stages.prefilter.fullPixels);
    checkRgba16BitExactAndFinite(
        reference,
        readbackRgba16FloatTexture(
            graphics.context.Get(),
            target.texture.Get()));

    const FxRenderCpuDiagnostics fullScreen = renderer.render(
        snapshot,
        target.view.Get());
    BAFX_CHECK(
        fullScreen.primaryActiveFxRoi.actualPath
        == FxActiveRoiActualPath::FullScreen);
    BAFX_CHECK(
        fullScreen.primaryActiveFxRoi.drawnPixels
        == fullScreen.primaryActiveFxRoi.fullPixels);
    checkRgba16BitExactAndFinite(
        reference,
        readbackRgba16FloatTexture(
            graphics.context.Get(),
            target.texture.Get()));

    const FxRenderCpuDiagnostics roiAfterFullScreen = renderer.render(
        snapshot,
        target.view.Get(),
        std::nullopt,
        nullptr,
        nullptr,
        roi);
    BAFX_CHECK(roiAfterFullScreen.primaryActiveFxRoi.warmup);
    BAFX_CHECK(
        roiAfterFullScreen.primaryActiveFxRoi.actualPath
        == FxActiveRoiActualPath::RoiWarmup);
    BAFX_CHECK(
        roiAfterFullScreen.primaryActiveFxRoi.stages.prefilter.clearedPixels
        == roiAfterFullScreen.primaryActiveFxRoi.stages.prefilter.fullPixels);
    checkRgba16BitExactAndFinite(
        reference,
        readbackRgba16FloatTexture(
            graphics.context.Get(),
            target.texture.Get()));

    const FxRenderCpuDiagnostics idle = renderer.render(
        bafx::fx::FrameSnapshot{},
        target.view.Get(),
        std::nullopt,
        nullptr,
        nullptr,
        roi);
    BAFX_CHECK(
        idle.primaryActiveFxRoi.actualPath == FxActiveRoiActualPath::Idle);
    BAFX_CHECK(
        idle.primaryActiveFxRoi.decisionReason
        == FxActiveRoiDecisionReason::NoContent);
    const Rgba16FloatImage empty = readbackRgba16FloatTexture(
        graphics.context.Get(),
        target.texture.Get());
    BAFX_CHECK(isZeroImage(empty));

    const FxRenderCpuDiagnostics restarted = renderer.render(
        snapshot,
        target.view.Get(),
        std::nullopt,
        nullptr,
        nullptr,
        roi);
    BAFX_CHECK(restarted.primaryActiveFxRoi.warmup);
    BAFX_CHECK(
        restarted.primaryActiveFxRoi.actualPath
        == FxActiveRoiActualPath::RoiWarmup);
    BAFX_CHECK(
        restarted.primaryActiveFxRoi.stages.prefilter.clearedPixels
        == restarted.primaryActiveFxRoi.stages.prefilter.fullPixels);
    checkRgba16BitExactAndFinite(
        reference,
        readbackRgba16FloatTexture(
            graphics.context.Get(),
            target.texture.Get()));

    const FxGpuFrameCapture referenceCapture = referenceRenderer.renderAndCapture(
        snapshot,
        referenceTarget.view.Get());
    const FxGpuFrameCapture restartedCapture = renderer.renderAndCapture(
        snapshot,
        target.view.Get(),
        roi);
    checkCaptureBitExactAndFinite(referenceCapture, restartedCapture);
}

BAFX_TEST(warp_active_fx_roi_edges_and_corners_match_four_tap_full_screen)
{
    struct BoundaryCase final
    {
        bafx::fx::PointF center{};
    };

    constexpr std::array<BoundaryCase, 8U> cases{
        BoundaryCase{bafx::fx::PointF{32.0F, 128.0F}},
        BoundaryCase{bafx::fx::PointF{224.0F, 128.0F}},
        BoundaryCase{bafx::fx::PointF{128.0F, 32.0F}},
        BoundaryCase{bafx::fx::PointF{128.0F, 224.0F}},
        BoundaryCase{bafx::fx::PointF{32.0F, 32.0F}},
        BoundaryCase{bafx::fx::PointF{224.0F, 32.0F}},
        BoundaryCase{bafx::fx::PointF{32.0F, 224.0F}},
        BoundaryCase{bafx::fx::PointF{224.0F, 224.0F}}};

    ComApartment apartment;
    const WarpDevice graphics = createWarpDevice();
    constexpr FxBloomSettings fourTapBloom{1.0F, 4.0F};
    FxGpuRenderer referenceRenderer(
        graphics.device.Get(),
        graphics.context.Get(),
        testSize,
        fourTapBloom);
    FxGpuRenderer roiRenderer(
        graphics.device.Get(),
        graphics.context.Get(),
        testSize,
        fourTapBloom);

    for (const BoundaryCase& testCase : cases)
    {
        bafx::fx::FrameSnapshot snapshot = makeDiskSnapshot(true);
        snapshot.sprites.front().centerPixels = testCase.center;
        const RenderTarget referenceTarget = createRenderTarget(
            graphics.device.Get());
        const RenderTarget roiTarget = createRenderTarget(graphics.device.Get());
        referenceRenderer.render(snapshot, referenceTarget.view.Get());
        const FxRenderCpuDiagnostics diagnostics = roiRenderer.render(
            snapshot,
            roiTarget.view.Get(),
            std::nullopt,
            nullptr,
            nullptr,
            makeActiveFxRoi(snapshot, fourTapBloom));

        const bool fellBackToFullScreen =
            diagnostics.primaryActiveFxRoi.actualPath
            == FxActiveRoiActualPath::FullScreen;
        const bool appliedRoi =
            diagnostics.primaryActiveFxRoi.actualPath
                == FxActiveRoiActualPath::RoiWarmup
            || diagnostics.primaryActiveFxRoi.actualPath
                == FxActiveRoiActualPath::RoiPyramid;
        BAFX_CHECK(fellBackToFullScreen || appliedRoi);
        if (fellBackToFullScreen)
        {
            BAFX_CHECK(
                diagnostics.primaryActiveFxRoi.drawnPixels
                == diagnostics.primaryActiveFxRoi.fullPixels);
        }
        checkRgba16BitExactAndFinite(
            readbackRgba16FloatTexture(
                graphics.context.Get(),
                referenceTarget.texture.Get()),
            readbackRgba16FloatTexture(
                graphics.context.Get(),
                roiTarget.texture.Get()));
    }
}

BAFX_TEST(warp_active_fx_roi_spout_background_matrix_keeps_paths_separate)
{
    ComApartment apartment;
    const WarpDevice graphics = createWarpDevice();
    constexpr FxBloomSettings fourTapBloom{1.0F, 4.0F};
    const bafx::fx::FrameSnapshot snapshot = makeDiskSnapshot(true);
    const FxActiveRoi roi = makeActiveFxRoi(snapshot, fourTapBloom);
    const RenderTarget desktopTarget = createRenderTarget(graphics.device.Get());
    const RenderTarget recordingTarget = createRecordingRenderTarget(
        graphics.device.Get());
    const RenderTarget backgroundTarget = createRenderTarget(
        graphics.device.Get());
    constexpr std::array<float, 4> background{0.25F, 0.5F, 0.75F, 1.0F};
    graphics.context->ClearRenderTargetView(
        backgroundTarget.view.Get(),
        background.data());

    FxGpuRenderer renderer(
        graphics.device.Get(),
        graphics.context.Get(),
        testSize,
        fourTapBloom);
    const FxRenderCpuDiagnostics fxOnly = renderer.render(
        snapshot,
        desktopTarget.view.Get(),
        std::nullopt,
        nullptr,
        nullptr,
        roi);
    BAFX_CHECK(
        fxOnly.primaryActiveFxRoi.actualPath
        == FxActiveRoiActualPath::RoiWarmup);
    BAFX_CHECK(!fxOnly.recordingRebuildActiveFxRoi.executed);
    BAFX_CHECK(
        fxOnly.recordingRebuildActiveFxRoi.actualPath
        == FxActiveRoiActualPath::Disabled);

    const FxRenderCpuDiagnostics spoutFxOnly = renderer.render(
        snapshot,
        desktopTarget.view.Get(),
        std::nullopt,
        nullptr,
        recordingTarget.view.Get(),
        roi);
    BAFX_CHECK(
        spoutFxOnly.primaryActiveFxRoi.actualPath
        == FxActiveRoiActualPath::RoiPyramid);
    BAFX_CHECK(!spoutFxOnly.recordingRebuildActiveFxRoi.executed);
    BAFX_CHECK(
        spoutFxOnly.recordingRebuildActiveFxRoi.actualPath
        == FxActiveRoiActualPath::Disabled);

    const FxRenderCpuDiagnostics backgroundOnly = renderer.render(
        snapshot,
        desktopTarget.view.Get(),
        BackgroundRenderInput{backgroundTarget.shaderResource.Get()},
        nullptr,
        nullptr,
        roi);
    BAFX_CHECK(
        backgroundOnly.primaryActiveFxRoi.actualPath
        == FxActiveRoiActualPath::FullScreen);
    BAFX_CHECK(
        backgroundOnly.primaryActiveFxRoi.decisionReason
        == FxActiveRoiDecisionReason::BackgroundDifferentialBloom);
    BAFX_CHECK(!backgroundOnly.recordingRebuildActiveFxRoi.executed);

    const FxRenderCpuDiagnostics backgroundSpout = renderer.render(
        snapshot,
        desktopTarget.view.Get(),
        BackgroundRenderInput{backgroundTarget.shaderResource.Get()},
        nullptr,
        recordingTarget.view.Get(),
        roi);
    BAFX_CHECK(
        backgroundSpout.primaryActiveFxRoi.actualPath
        == FxActiveRoiActualPath::FullScreen);
    BAFX_CHECK(
        backgroundSpout.primaryActiveFxRoi.decisionReason
        == FxActiveRoiDecisionReason::BackgroundDifferentialBloom);
    BAFX_CHECK(
        backgroundSpout.recordingRebuildActiveFxRoi.actualPath
        == FxActiveRoiActualPath::RoiWarmup);
    BAFX_CHECK(backgroundSpout.recordingRebuildActiveFxRoi.warmup);
    BAFX_CHECK(
        backgroundSpout.recordingRebuildActiveFxRoi.decisionReason
        == FxActiveRoiDecisionReason::SharedTargetFullWrite);

    const FxRenderCpuDiagnostics repeatedBackgroundSpout = renderer.render(
        snapshot,
        desktopTarget.view.Get(),
        BackgroundRenderInput{backgroundTarget.shaderResource.Get()},
        nullptr,
        recordingTarget.view.Get(),
        roi);
    BAFX_CHECK(
        repeatedBackgroundSpout.primaryActiveFxRoi.actualPath
        == FxActiveRoiActualPath::FullScreen);
    BAFX_CHECK(
        repeatedBackgroundSpout.recordingRebuildActiveFxRoi.actualPath
        == FxActiveRoiActualPath::RoiWarmup);
    BAFX_CHECK(
        repeatedBackgroundSpout.recordingRebuildActiveFxRoi.decisionReason
        == FxActiveRoiDecisionReason::SharedTargetFullWrite);

    const FxRenderCpuDiagnostics returnedToFxOnly = renderer.render(
        snapshot,
        desktopTarget.view.Get(),
        std::nullopt,
        nullptr,
        recordingTarget.view.Get(),
        roi);
    BAFX_CHECK(
        returnedToFxOnly.primaryActiveFxRoi.actualPath
        == FxActiveRoiActualPath::RoiPyramid);
    BAFX_CHECK(
        returnedToFxOnly.primaryActiveFxRoi.decisionReason
        == FxActiveRoiDecisionReason::Applied);
    BAFX_CHECK(
        returnedToFxOnly.primaryActiveFxRoi.stages.prefilter.clearedPixels
        == returnedToFxOnly.primaryActiveFxRoi.stages.prefilter.candidatePixels);
    BAFX_CHECK(
        returnedToFxOnly.primaryActiveFxRoi.stages.downsample.clearedPixels
        == returnedToFxOnly.primaryActiveFxRoi.stages.downsample.candidatePixels);
    BAFX_CHECK(
        returnedToFxOnly.primaryActiveFxRoi.stages.upsample.clearedPixels
        == returnedToFxOnly.primaryActiveFxRoi.stages.upsample.candidatePixels);
    BAFX_CHECK(!returnedToFxOnly.recordingRebuildActiveFxRoi.executed);
    BAFX_CHECK(
        returnedToFxOnly.recordingRebuildActiveFxRoi.actualPath
        == FxActiveRoiActualPath::Disabled);
}

BAFX_TEST(warp_active_fx_roi_negative_scrgb_and_hdr_extremes_are_exact)
{
    constexpr std::array<std::array<float, 4U>, 2U> backgrounds{
        std::array<float, 4U>{-0.5F, -0.03125F, 0.5F, 1.0F},
        std::array<float, 4U>{64.0F, 4096.0F, 65504.0F, 1.0F}};

    ComApartment apartment;
    const WarpDevice graphics = createWarpDevice();
    constexpr FxBloomSettings fourTapBloom{1.0F, 4.0F};
    const bafx::fx::FrameSnapshot snapshot = makeDiskSnapshot(true);
    const FxActiveRoi roi = makeActiveFxRoi(snapshot, fourTapBloom);

    for (const std::array<float, 4U>& background : backgrounds)
    {
        const RenderTarget backgroundTarget = createRenderTarget(
            graphics.device.Get());
        graphics.context->ClearRenderTargetView(
            backgroundTarget.view.Get(),
            background.data());

        FxGpuRenderer referenceRenderer(
            graphics.device.Get(),
            graphics.context.Get(),
            testSize,
            fourTapBloom);
        const RenderTarget referenceDesktop = createRenderTarget(
            graphics.device.Get());
        const RenderTarget referenceRecording = createRecordingRenderTarget(
            graphics.device.Get());
        referenceRenderer.render(
            snapshot,
            referenceDesktop.view.Get(),
            BackgroundRenderInput{backgroundTarget.shaderResource.Get()},
            nullptr,
            referenceRecording.view.Get());
        const Rgba16FloatImage expectedDesktop = readbackRgba16FloatTexture(
            graphics.context.Get(),
            referenceDesktop.texture.Get());
        const Bgra8Image expectedRecording = readbackBgra8(
            graphics.context.Get(),
            referenceRecording.texture.Get());

        FxGpuRenderer roiRenderer(
            graphics.device.Get(),
            graphics.context.Get(),
            testSize,
            fourTapBloom);
        const RenderTarget roiDesktop = createRenderTarget(graphics.device.Get());
        const RenderTarget roiRecording = createRecordingRenderTarget(
            graphics.device.Get());
        const FxRenderCpuDiagnostics diagnostics = roiRenderer.render(
            snapshot,
            roiDesktop.view.Get(),
            BackgroundRenderInput{backgroundTarget.shaderResource.Get()},
            nullptr,
            roiRecording.view.Get(),
            roi);

        BAFX_CHECK(
            diagnostics.primaryActiveFxRoi.actualPath
            == FxActiveRoiActualPath::FullScreen);
        BAFX_CHECK(
            diagnostics.primaryActiveFxRoi.decisionReason
            == FxActiveRoiDecisionReason::BackgroundDifferentialBloom);
        BAFX_CHECK(
            diagnostics.recordingRebuildActiveFxRoi.actualPath
            == FxActiveRoiActualPath::RoiWarmup);
        BAFX_CHECK(
            diagnostics.recordingRebuildActiveFxRoi.decisionReason
            == FxActiveRoiDecisionReason::SharedTargetFullWrite);
        checkRgba16BitExactAndFinite(
            expectedDesktop,
            readbackRgba16FloatTexture(
                graphics.context.Get(),
                roiDesktop.texture.Get()));
        BAFX_CHECK(sameBgra8(
            expectedRecording,
            readbackBgra8(
                graphics.context.Get(),
                roiRecording.texture.Get())));

        const FxRenderCpuDiagnostics repeated = roiRenderer.render(
            snapshot,
            roiDesktop.view.Get(),
            BackgroundRenderInput{backgroundTarget.shaderResource.Get()},
            nullptr,
            roiRecording.view.Get(),
            roi);
        BAFX_CHECK(
            repeated.primaryActiveFxRoi.actualPath
            == FxActiveRoiActualPath::FullScreen);
        BAFX_CHECK(
            repeated.primaryActiveFxRoi.decisionReason
            == FxActiveRoiDecisionReason::BackgroundDifferentialBloom);
        BAFX_CHECK(
            repeated.recordingRebuildActiveFxRoi.actualPath
            == FxActiveRoiActualPath::RoiWarmup);
        BAFX_CHECK(
            repeated.recordingRebuildActiveFxRoi.decisionReason
            == FxActiveRoiDecisionReason::SharedTargetFullWrite);
        checkRgba16BitExactAndFinite(
            expectedDesktop,
            readbackRgba16FloatTexture(
                graphics.context.Get(),
                roiDesktop.texture.Get()));
        BAFX_CHECK(sameBgra8(
            expectedRecording,
            readbackBgra8(
                graphics.context.Get(),
                roiRecording.texture.Get())));
    }
}

BAFX_TEST(warp_capture_proves_triangle_enters_bloom)
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
    BAFX_CHECK(!isZeroImage(capture.bloomSeed));
    BAFX_CHECK(!isZeroImage(capture.bloomResult));
    bool hasBloomDown = false;
    for (const Rgba16FloatImage& mip : capture.bloomDown)
    {
        hasBloomDown = hasBloomDown || !isZeroImage(mip);
    }
    BAFX_CHECK(hasBloomDown);
    bool hasBloomUp = false;
    for (const Rgba16FloatImage& mip : capture.bloomUp)
    {
        hasBloomUp = hasBloomUp || !isZeroImage(mip);
    }
    BAFX_CHECK(hasBloomUp);
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
    BAFX_CHECK(capture.bloomResult.pixels.empty());
    BAFX_CHECK(isZeroImage(capture.finalOverlay));
}

BAFX_TEST(warp_spout2_recording_target_exports_fx_without_wgc_background)
{
    ComApartment apartment;
    const WarpDevice graphics = createWarpDevice();
    FxGpuRenderer renderer(graphics.device.Get(), graphics.context.Get(), testSize);
    const RenderTarget desktopTarget = createRenderTarget(graphics.device.Get());
    const RenderTarget recordingTarget = createRecordingRenderTarget(
        graphics.device.Get());

    renderer.render(
        makeDiskAndTrailSnapshot(),
        desktopTarget.view.Get(),
        std::nullopt,
        nullptr,
        recordingTarget.view.Get());

    const Bgra8UnormPixel center = readbackBgra8UnormPixel(
        graphics.context.Get(),
        recordingTarget.texture.Get(),
        testSize.width / 2U,
        testSize.height / 2U);
    const Bgra8UnormPixel background = readbackBgra8UnormPixel(
        graphics.context.Get(),
        recordingTarget.texture.Get(),
        testSize.width - 1U,
        testSize.height - 1U);
    const Bgra8Image image = readbackBgra8(
        graphics.context.Get(),
        recordingTarget.texture.Get());
    BAFX_CHECK(center.alpha > 0U);
    BAFX_CHECK(hasExtendedEmission(image));
    BAFX_CHECK(!hasZeroAlphaEmission(image));
    BAFX_CHECK(background.alpha == 0U);
    BAFX_CHECK(
        center.red > background.red
        || center.green > background.green
        || center.blue > background.blue);
    BAFX_CHECK(background.red == 0U);
    BAFX_CHECK(background.green == 0U);
    BAFX_CHECK(background.blue == 0U);
}

BAFX_TEST(warp_spout2_full_applies_sdr_rolloff_without_srgb_uplift)
{
    ComApartment apartment;
    const WarpDevice graphics = createWarpDevice();
    FxGpuRenderer renderer(graphics.device.Get(), graphics.context.Get(), testSize);
    const RenderTarget captureTarget = createRenderTarget(graphics.device.Get());
    const RenderTarget desktopTarget = createRenderTarget(graphics.device.Get());
    const RenderTarget recordingTarget = createRecordingRenderTarget(
        graphics.device.Get());
    const bafx::fx::FrameSnapshot snapshot = makeTriangleTransportSnapshot(
        1.0F,
        0.5F);

    const FxGpuFrameCapture capture = renderer.renderAndCapture(
        snapshot,
        captureTarget.view.Get());
    renderer.render(
        snapshot,
        desktopTarget.view.Get(),
        std::nullopt,
        nullptr,
        recordingTarget.view.Get());

    BAFX_CHECK(capture.intermediateLayersValid);
    checkObsSdrRolloffEncoding(
        readbackBgra8(graphics.context.Get(), recordingTarget.texture.Get()),
        capture.directSurface,
        &capture.bloomResult);
}

BAFX_TEST(warp_spout2_additive_layers_export_alpha_backed_emission)
{
    ComApartment apartment;
    const WarpDevice graphics = createWarpDevice();
    FxGpuRenderer renderer(graphics.device.Get(), graphics.context.Get(), testSize);
    const RenderTarget desktopTarget = createRenderTarget(graphics.device.Get());
    const RenderTarget recordingTarget = createRecordingRenderTarget(
        graphics.device.Get());

    const std::array snapshots{
        makeDissolveRingSnapshot(),
        makeTriangleSnapshot(),
        makeTwoTrailSnapshot()};
    for (const bafx::fx::FrameSnapshot& snapshot : snapshots)
    {
        renderer.render(
            snapshot,
            desktopTarget.view.Get(),
            std::nullopt,
            nullptr,
            recordingTarget.view.Get());
        const Bgra8Image image = readbackBgra8(
            graphics.context.Get(),
            recordingTarget.texture.Get());

        BAFX_CHECK(!hasZeroAlphaEmission(image));
        const std::uint8_t maximumAlpha = std::max_element(
            image.pixels.begin(),
            image.pixels.end(),
            [](const Bgra8UnormPixel left, const Bgra8UnormPixel right)
            {
                return left.alpha < right.alpha;
            })->alpha;
        // One UNORM step keeps additive pixels alive while changing an OBS
        // background by at most one byte under normal premultiplied blending.
        BAFX_CHECK(maximumAlpha == 1U);
        const std::size_t emissionPixels = static_cast<std::size_t>(
            std::count_if(
                image.pixels.begin(),
                image.pixels.end(),
                [](const Bgra8UnormPixel pixel)
                {
                    return pixel.alpha > 0U
                        && (pixel.red != 0U
                            || pixel.green != 0U
                            || pixel.blue != 0U);
                }));
        BAFX_CHECK(emissionPixels > 100U);
    }
}

BAFX_TEST(warp_spout2_recording_target_replaces_idle_frame_with_transparency)
{
    ComApartment apartment;
    const WarpDevice graphics = createWarpDevice();
    FxGpuRenderer renderer(graphics.device.Get(), graphics.context.Get(), testSize);
    const RenderTarget desktopTarget = createRenderTarget(graphics.device.Get());
    const RenderTarget recordingTarget = createRecordingRenderTarget(
        graphics.device.Get());

    renderer.render(
        makeDiskSnapshot(true),
        desktopTarget.view.Get(),
        std::nullopt,
        nullptr,
        recordingTarget.view.Get());
    renderer.render(
        bafx::fx::FrameSnapshot{},
        desktopTarget.view.Get(),
        std::nullopt,
        nullptr,
        recordingTarget.view.Get());

    const Bgra8UnormPixel center = readbackBgra8UnormPixel(
        graphics.context.Get(),
        recordingTarget.texture.Get(),
        testSize.width / 2U,
        testSize.height / 2U);
    const Bgra8Image image = readbackBgra8(
        graphics.context.Get(),
        recordingTarget.texture.Get());
    BAFX_CHECK(center.red == 0U);
    BAFX_CHECK(center.green == 0U);
    BAFX_CHECK(center.blue == 0U);
    BAFX_CHECK(center.alpha == 0U);
    BAFX_CHECK(isTransparent(image));
}

BAFX_TEST(warp_spout2_recording_target_never_flattens_wgc_background)
{
    ComApartment apartment;
    const WarpDevice graphics = createWarpDevice();
    FxGpuRenderer renderer(graphics.device.Get(), graphics.context.Get(), testSize);
    const RenderTarget desktopTarget = createRenderTarget(graphics.device.Get());
    const RenderTarget recordingTarget = createRecordingRenderTarget(
        graphics.device.Get());
    const RenderTarget referenceRecordingTarget = createRecordingRenderTarget(
        graphics.device.Get());
    const RenderTarget backgroundTarget = createRenderTarget(graphics.device.Get());
    constexpr std::array<float, 4> brightBackground{0.8F, 0.6F, 0.4F, 1.0F};
    graphics.context->ClearRenderTargetView(
        backgroundTarget.view.Get(),
        brightBackground.data());

    renderer.render(
        makeDiskAndTrailSnapshot(),
        desktopTarget.view.Get(),
        std::nullopt,
        nullptr,
        referenceRecordingTarget.view.Get());
    renderer.render(
        makeDiskAndTrailSnapshot(),
        desktopTarget.view.Get(),
        BackgroundRenderInput{backgroundTarget.shaderResource.Get()},
        nullptr,
        recordingTarget.view.Get());

    const Bgra8UnormPixel center = readbackBgra8UnormPixel(
        graphics.context.Get(),
        recordingTarget.texture.Get(),
        testSize.width / 2U,
        testSize.height / 2U);
    const Bgra8UnormPixel background = readbackBgra8UnormPixel(
        graphics.context.Get(),
        recordingTarget.texture.Get(),
        testSize.width - 1U,
        testSize.height - 1U);
    const Bgra8Image image = readbackBgra8(
        graphics.context.Get(),
        recordingTarget.texture.Get());
    const Bgra8Image referenceImage = readbackBgra8(
        graphics.context.Get(),
        referenceRecordingTarget.texture.Get());
    BAFX_CHECK(sameBgra8(image, referenceImage));
    BAFX_CHECK(center.alpha > 0U);
    BAFX_CHECK(hasExtendedEmission(image));
    BAFX_CHECK(!hasZeroAlphaEmission(image));
    BAFX_CHECK(background.red == 0U);
    BAFX_CHECK(background.green == 0U);
    BAFX_CHECK(background.blue == 0U);
    BAFX_CHECK(background.alpha == 0U);
}

BAFX_TEST(warp_core_profile_exports_spout2_fx_without_bloom)
{
    ComApartment apartment;
    const WarpDevice graphics = createWarpDevice();
    FxGpuRenderer renderer(graphics.device.Get(), graphics.context.Get(), testSize);
    renderer.setOverlayProfile(FxOverlayProfile::Core);
    const RenderTarget desktopTarget = createRenderTarget(graphics.device.Get());
    const RenderTarget recordingTarget = createRecordingRenderTarget(
        graphics.device.Get());

    renderer.render(
        makeDiskAndTrailSnapshot(),
        desktopTarget.view.Get(),
        std::nullopt,
        nullptr,
        recordingTarget.view.Get());

    const Bgra8UnormPixel center = readbackBgra8UnormPixel(
        graphics.context.Get(),
        recordingTarget.texture.Get(),
        testSize.width / 2U,
        testSize.height / 2U);
    const Bgra8UnormPixel trail = readbackBgra8UnormPixel(
        graphics.context.Get(),
        recordingTarget.texture.Get(),
        64U,
        64U);
    BAFX_CHECK(center.alpha > 0U);
    BAFX_CHECK(trail.alpha == 1U);
    BAFX_CHECK(center.red != 0U || center.green != 0U || center.blue != 0U);
    BAFX_CHECK(trail.red != 0U || trail.green != 0U || trail.blue != 0U);
    BAFX_CHECK(
        trail.red > trail.alpha
        || trail.green > trail.alpha
        || trail.blue > trail.alpha);
}

BAFX_TEST(warp_core_spout2_applies_sdr_rolloff_without_srgb_uplift)
{
    ComApartment apartment;
    const WarpDevice graphics = createWarpDevice();
    FxGpuRenderer renderer(graphics.device.Get(), graphics.context.Get(), testSize);
    renderer.setOverlayProfile(FxOverlayProfile::Core);
    const RenderTarget captureTarget = createRenderTarget(graphics.device.Get());
    const RenderTarget desktopTarget = createRenderTarget(graphics.device.Get());
    const RenderTarget recordingTarget = createRecordingRenderTarget(
        graphics.device.Get());
    const bafx::fx::FrameSnapshot snapshot = makeTriangleTransportSnapshot(
        1.0F,
        0.5F);

    const FxGpuFrameCapture capture = renderer.renderAndCapture(
        snapshot,
        captureTarget.view.Get());
    renderer.render(
        snapshot,
        desktopTarget.view.Get(),
        std::nullopt,
        nullptr,
        recordingTarget.view.Get());

    BAFX_CHECK(!capture.directSurface.pixels.empty());
    BAFX_CHECK(capture.bloomResult.pixels.empty());
    checkObsSdrRolloffEncoding(
        readbackBgra8(graphics.context.Get(), recordingTarget.texture.Get()),
        capture.directSurface);
}

BAFX_TEST(warp_core_spout2_ignores_wgc_and_clears_after_fx_decay)
{
    ComApartment apartment;
    const WarpDevice graphics = createWarpDevice();
    FxGpuRenderer renderer(graphics.device.Get(), graphics.context.Get(), testSize);
    renderer.setOverlayProfile(FxOverlayProfile::Core);
    const RenderTarget desktopTarget = createRenderTarget(graphics.device.Get());
    const RenderTarget recordingTarget = createRecordingRenderTarget(
        graphics.device.Get());
    const RenderTarget backgroundTarget = createRenderTarget(graphics.device.Get());
    constexpr std::array<float, 4> brightBackground{0.9F, 0.7F, 0.5F, 1.0F};
    graphics.context->ClearRenderTargetView(
        backgroundTarget.view.Get(),
        brightBackground.data());

    renderer.render(
        makeDiskAndTrailSnapshot(),
        desktopTarget.view.Get(),
        BackgroundRenderInput{backgroundTarget.shaderResource.Get()},
        nullptr,
        recordingTarget.view.Get());
    const Bgra8UnormPixel active = readbackBgra8UnormPixel(
        graphics.context.Get(),
        recordingTarget.texture.Get(),
        testSize.width / 2U,
        testSize.height / 2U);
    const Bgra8UnormPixel activeBackground = readbackBgra8UnormPixel(
        graphics.context.Get(),
        recordingTarget.texture.Get(),
        testSize.width - 1U,
        testSize.height - 1U);
    BAFX_CHECK(active.alpha > 0U);
    BAFX_CHECK(activeBackground.red == 0U);
    BAFX_CHECK(activeBackground.green == 0U);
    BAFX_CHECK(activeBackground.blue == 0U);
    BAFX_CHECK(activeBackground.alpha == 0U);

    renderer.render(
        bafx::fx::FrameSnapshot{},
        desktopTarget.view.Get(),
        BackgroundRenderInput{backgroundTarget.shaderResource.Get()},
        nullptr,
        recordingTarget.view.Get());
    const Bgra8UnormPixel decayed = readbackBgra8UnormPixel(
        graphics.context.Get(),
        recordingTarget.texture.Get(),
        testSize.width / 2U,
        testSize.height / 2U);
    const Bgra8Image decayedImage = readbackBgra8(
        graphics.context.Get(),
        recordingTarget.texture.Get());
    BAFX_CHECK(decayed.red == 0U);
    BAFX_CHECK(decayed.green == 0U);
    BAFX_CHECK(decayed.blue == 0U);
    BAFX_CHECK(decayed.alpha == 0U);
    BAFX_CHECK(isTransparent(decayedImage));
}

BAFX_TEST(warp_core_profile_keeps_trail_without_bloom_layers)
{
    ComApartment apartment;
    const WarpDevice graphics = createWarpDevice();
    FxGpuRenderer renderer(graphics.device.Get(), graphics.context.Get(), testSize);
    renderer.setOverlayProfile(FxOverlayProfile::Core);
    const RenderTarget target = createRenderTarget(graphics.device.Get());

    const FxGpuFrameCapture capture = renderer.renderAndCapture(
        makeDiskAndTrailSnapshot(),
        target.view.Get());

    BAFX_CHECK(!capture.directSurface.pixels.empty());
    BAFX_CHECK(capture.bloomSeed.pixels.empty());
    BAFX_CHECK(capture.bloomDown.empty());
    BAFX_CHECK(capture.bloomUp.empty());
    BAFX_CHECK(capture.bloomResult.pixels.empty());
    BAFX_CHECK(
        maximumRgbOutsideSprite(toFloatPixels(capture.finalOverlay))
        > 1.0e-3F);
}
