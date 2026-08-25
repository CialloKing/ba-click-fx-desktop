#include "bafx/windows/fx_gpu_renderer.hpp"

#include "bafx/windows/gpu_timestamp_profiler.hpp"

#include "bafx/core/color_space.hpp"
#include "bafx/core/roi.hpp"
#include "bafx/core/theme_color.hpp"
#include "bafx/core/unity_bloom.hpp"
#include "bafx/core/unity_ring_mesh.hpp"
#include "bafx/core/unity_trail_mesh.hpp"
#include "bafx/windows/error.hpp"
#include "bafx/windows/gpu_texture_readback.hpp"
#include "embedded_fx_shaders.hpp"
#include "packed_fx_texture_loader.hpp"

#include <d3d11_1.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace bafx::windows
{
namespace
{

using Microsoft::WRL::ComPtr;

constexpr float trailArtisticIntensity = 23.968628F;
constexpr std::int32_t trailRenderQueue = 4499;
constexpr float trailCoverageFadeStart = 0.248532F;
constexpr float trailCoverageFadeEnd = 0.97941558F;
constexpr std::size_t initialVertexCapacity = bafx::core::unityRingIndexCount;
constexpr float minimumBloomDiffusion = 0.0F;
constexpr float maximumBloomDiffusion = 10.0F;
constexpr float maximumBloomIntensity = 10.0F;
constexpr float scRgbNitsPerUnit = 80.0F;

struct SpriteVertex
{
    float position[2]{};
    float uv[2]{};
    float color[4]{};
    float intensity{1.0F};
    float dissolveThreshold{0.0F};
    float bloomEnabled{0.0F};
    float coverageFactor{1.0F};
    float globalOpacity{1.0F};
};

struct ViewportConstants
{
    float size[2]{};
    float padding[2]{};
};

struct BloomConstants
{
    float sourceTexelSize[2]{};
    float sampleScale{1.0F};
    float exposureGain{0.0F};
    float threshold{1.0F};
    float knee{0.00001F};
    float clampValue{65472.0F};
    float backgroundTransportEnabled{0.0F};
    float backgroundReferenceWhiteScale{1.0F};
    float outputReferenceWhiteScale{1.0F};
    float themeCoverageScale{1.0F};
    float padding{0.0F};
};

static_assert(sizeof(BloomConstants) == 48U);

struct BloomResolveRoiConstants
{
    std::int32_t rect[4]{};
};

static_assert(sizeof(BloomResolveRoiConstants) == 16U);

struct ColorTarget
{
    ComPtr<ID3D11Texture2D> texture{};
    ComPtr<ID3D11RenderTargetView> renderTarget{};
    ComPtr<ID3D11ShaderResourceView> shaderResource{};
};

[[nodiscard]] bool hasValidBloomSettings(const FxBloomSettings settings) noexcept
{
    return std::isfinite(settings.intensity)
        && settings.intensity >= 0.0F
        && settings.intensity <= maximumBloomIntensity
        && std::isfinite(settings.diffusion)
        && settings.diffusion >= minimumBloomDiffusion
        && settings.diffusion <= maximumBloomDiffusion
        && std::isfinite(settings.threshold)
        && settings.threshold >= 0.0F
        && settings.threshold <= 64.0F
        && std::isfinite(settings.softKnee)
        && settings.softKnee >= 0.0F
        && settings.softKnee <= 1.0F
        && std::isfinite(settings.clampValue)
        && settings.clampValue >= 0.0F
        && settings.clampValue <= 65504.0F;
}

[[nodiscard]] bool sameBloomSettings(
    const FxBloomSettings left,
    const FxBloomSettings right) noexcept
{
    return left.intensity == right.intensity
        && left.diffusion == right.diffusion
        && left.threshold == right.threshold
        && left.softKnee == right.softKnee
        && left.clampValue == right.clampValue
        && left.enabled == right.enabled;
}

[[nodiscard]] bool supportsActiveFxRoiClearView(
    ID3D11Device* const device,
    const std::optional<bool> capabilityOverride) noexcept
{
    if (capabilityOverride.has_value())
    {
        return *capabilityOverride;
    }
    if (device == nullptr)
    {
        return false;
    }

    D3D11_FEATURE_DATA_D3D11_OPTIONS options{};
    return SUCCEEDED(device->CheckFeatureSupport(
               D3D11_FEATURE_D3D11_OPTIONS,
               &options,
               sizeof(options)))
        && options.ClearView == TRUE;
}

[[nodiscard]] bool isValidOverlayProfile(
    const FxOverlayProfile profile) noexcept
{
    switch (profile)
    {
    case FxOverlayProfile::FxOnlyFallback:
    case FxOverlayProfile::RecordingCompatible:
    case FxOverlayProfile::LightBackground:
    case FxOverlayProfile::Core:
        return true;
    }
    return false;
}

[[nodiscard]] ComPtr<ID3DBlob> compileShader(
    const std::string_view source,
    const char* entryPoint,
    const char* profile)
{
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
    flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

    ComPtr<ID3DBlob> byteCode;
    ComPtr<ID3DBlob> errors;
    const HRESULT result = D3DCompile(
        source.data(),
        source.size(),
        "embedded_fx_shaders.hpp",
        nullptr,
        nullptr,
        entryPoint,
        profile,
        flags,
        0,
        &byteCode,
        &errors);
    if (FAILED(result))
    {
        std::string message = "D3DCompile failed";
        if (errors != nullptr && errors->GetBufferPointer() != nullptr)
        {
            message += ": ";
            message.append(
                static_cast<const char*>(errors->GetBufferPointer()),
                errors->GetBufferSize());
        }
        throw std::runtime_error(message);
    }
    return byteCode;
}

[[nodiscard]] D3D11_RENDER_TARGET_BLEND_DESC additiveBlendTarget() noexcept
{
    D3D11_RENDER_TARGET_BLEND_DESC target{};
    target.BlendEnable = TRUE;
    target.SrcBlend = D3D11_BLEND_ONE;
    target.DestBlend = D3D11_BLEND_ONE;
    target.BlendOp = D3D11_BLEND_OP_ADD;
    // DComp needs accumulated coverage even though Unity only consumes additive RGB.
    target.SrcBlendAlpha = D3D11_BLEND_ONE;
    target.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    return target;
}

[[nodiscard]] D3D11_RENDER_TARGET_BLEND_DESC sourceOverBlendTarget() noexcept
{
    D3D11_RENDER_TARGET_BLEND_DESC target{};
    target.BlendEnable = TRUE;
    target.SrcBlend = D3D11_BLEND_ONE;
    target.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    target.BlendOp = D3D11_BLEND_OP_ADD;
    target.SrcBlendAlpha = D3D11_BLEND_ONE;
    target.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    return target;
}

[[nodiscard]] D3D11_RENDER_TARGET_BLEND_DESC coverageUnionBlendTarget() noexcept
{
    D3D11_RENDER_TARGET_BLEND_DESC target{};
    target.BlendEnable = TRUE;
    target.SrcBlend = D3D11_BLEND_ONE;
    target.DestBlend = D3D11_BLEND_INV_SRC_COLOR;
    target.BlendOp = D3D11_BLEND_OP_ADD;
    target.SrcBlendAlpha = D3D11_BLEND_ONE;
    target.DestBlendAlpha = D3D11_BLEND_ZERO;
    target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED
        | D3D11_COLOR_WRITE_ENABLE_GREEN;
    return target;
}

[[nodiscard]] ColorTarget createColorTarget(
    ID3D11Device* device,
    const WindowSize size,
    const DXGI_FORMAT format = DXGI_FORMAT_R16G16B16A16_FLOAT)
{
    D3D11_TEXTURE2D_DESC description{};
    description.Width = size.width;
    description.Height = size.height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = format;
    description.SampleDesc = DXGI_SAMPLE_DESC{1, 0};
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    ColorTarget target{};
    throwIfFailed(
        device->CreateTexture2D(&description, nullptr, &target.texture),
        "ID3D11Device::CreateTexture2D(FX target)");
    throwIfFailed(
        device->CreateRenderTargetView(target.texture.Get(), nullptr, &target.renderTarget),
        "ID3D11Device::CreateRenderTargetView(FX target)");
    throwIfFailed(
        device->CreateShaderResourceView(target.texture.Get(), nullptr, &target.shaderResource),
        "ID3D11Device::CreateShaderResourceView(FX target)");
    return target;
}

[[nodiscard]] ComPtr<ID3D11Texture2D> textureFromRenderTarget(
    ID3D11RenderTargetView* renderTarget)
{
    if (renderTarget == nullptr)
    {
        throw std::invalid_argument("FX render target is required");
    }

    ComPtr<ID3D11Resource> resource;
    renderTarget->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture;
    throwIfFailed(
        resource.As(&texture),
        "ID3D11RenderTargetView::GetResource(ID3D11Texture2D)");
    return texture;
}

[[nodiscard]] ComPtr<ID3D11Texture2D> textureFromShaderResource(
    ID3D11ShaderResourceView* shaderResource) noexcept
{
    if (shaderResource == nullptr)
    {
        return {};
    }

    ComPtr<ID3D11Resource> resource;
    shaderResource->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture;
    if (resource == nullptr || FAILED(resource.As(&texture)))
    {
        return {};
    }
    return texture;
}

[[nodiscard]] bool isCompatibleBackgroundTexture(
    ID3D11Texture2D* texture,
    const WindowSize size) noexcept
{
    if (texture == nullptr)
    {
        return false;
    }

    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    return description.Width == size.width
        && description.Height == size.height
        && description.MipLevels == 1U
        && description.ArraySize == 1U
        && description.Format == DXGI_FORMAT_R16G16B16A16_FLOAT
        && description.SampleDesc.Count == 1U
        && description.SampleDesc.Quality == 0U;
}

[[nodiscard]] bool hasVisualContent(
    const bafx::fx::FrameSnapshot& snapshot) noexcept
{
    return snapshot.hasDrawableContent();
}

[[nodiscard]] SpriteVertex makeVertex(
    const float x,
    const float y,
    const float u,
    const float v,
    const bafx::fx::ColorF color,
    const float intensity,
    const float dissolveThreshold,
    const bool contributesBloom,
    const float coverageFactor = 1.0F,
    const float globalOpacity = 1.0F) noexcept
{
    return SpriteVertex{
        {x, y},
        {u, v},
        {color.r, color.g, color.b, color.a},
        intensity,
        dissolveThreshold,
        contributesBloom ? 1.0F : 0.0F,
        coverageFactor,
        globalOpacity};
}

[[nodiscard]] float evaluateTrailLongitudinalCoverage(const float progress) noexcept
{
    if (progress <= trailCoverageFadeStart)
    {
        return 0.0F;
    }
    if (progress >= trailCoverageFadeEnd)
    {
        return 1.0F;
    }

    const float value = (progress - trailCoverageFadeStart)
        / (trailCoverageFadeEnd - trailCoverageFadeStart);
    // Match the WebGPU transport curve with flat derivatives at both
    // deletion boundaries.
    return value * value * value * (value * (value * 6.0F - 15.0F) + 10.0F);
}

[[nodiscard]] std::array<SpriteVertex, 6> makeSpriteVertices(
    const bafx::fx::Sprite& sprite,
    const float globalOpacity,
    const bafx::core::RelativeOklchTheme& theme) noexcept
{
    const float halfSize = sprite.sizePixels * 0.5F;
    const float cosine = std::cos(sprite.rotationRadians);
    const float sine = std::sin(sprite.rotationRadians);
    constexpr std::array<std::array<float, 2>, 4> corners{{
        {-1.0F, -1.0F},
        {1.0F, -1.0F},
        {1.0F, 1.0F},
        {-1.0F, 1.0F}}};
    std::array<std::array<float, 2>, 4> positions{};
    for (std::size_t index = 0; index < corners.size(); ++index)
    {
        const float localX = corners[index][0] * halfSize;
        const float localY = corners[index][1] * halfSize;
        positions[index] = {
            sprite.centerPixels.x + localX * cosine - localY * sine,
            sprite.centerPixels.y + localX * sine + localY * cosine};
    }

    float minimumU = 0.0F;
    float maximumU = 1.0F;
    if (sprite.kind == bafx::fx::SpriteKind::Triangle)
    {
        minimumU = sprite.atlasFrame == 0U ? 0.0F : 0.5F;
        maximumU = minimumU + 0.5F;
    }
    const std::array<std::array<float, 2>, 4> uvs{{
        {minimumU, 0.0F},
        {maximumU, 0.0F},
        {maximumU, 1.0F},
        {minimumU, 1.0F}}};

    const bafx::core::Float3 mappedColor = bafx::core::applyRelativeOklchTheme(
        bafx::core::Float3{sprite.color.r, sprite.color.g, sprite.color.b},
        theme);
    const auto vertex = [
                            &sprite,
                            &positions,
                            &uvs,
                            globalOpacity,
                            mappedColor](const std::size_t index)
    {
        return makeVertex(
            positions[index][0],
            positions[index][1],
            uvs[index][0],
            uvs[index][1],
            bafx::fx::ColorF{
                mappedColor.r,
                mappedColor.g,
                mappedColor.b,
                sprite.color.a},
            sprite.artisticIntensity,
            sprite.dissolveThreshold,
            sprite.contributesBloom,
            1.0F,
            globalOpacity);
    };
    return {vertex(0), vertex(1), vertex(2), vertex(0), vertex(2), vertex(3)};
}

[[nodiscard]] std::array<SpriteVertex, bafx::core::unityRingIndexCount>
makeRingVertices(
    const bafx::fx::Sprite& sprite,
    const float globalOpacity,
    const bafx::core::RelativeOklchTheme& theme) noexcept
{
    // Cylinder002 is regular, so its exact topology can remain code-generated.
    static const bafx::core::UnityRingMesh mesh = bafx::core::makeUnityRingMesh();
    std::array<SpriteVertex, bafx::core::unityRingIndexCount> vertices{};
    const float scale = sprite.sizePixels
        / (2.0F * bafx::core::unityRingOuterRadius);
    const float cosine = std::cos(sprite.rotationRadians);
    const float sine = std::sin(sprite.rotationRadians);
    const bafx::core::Float3 mappedColor = bafx::core::applyRelativeOklchTheme(
        bafx::core::Float3{sprite.color.r, sprite.color.g, sprite.color.b},
        theme);
    for (std::size_t index = 0U; index < mesh.indices.size(); ++index)
    {
        const bafx::core::UnityRingVertex& source =
            mesh.vertices[mesh.indices[index]];
        const float rotatedX = source.x * cosine - source.y * sine;
        const float rotatedY = source.x * sine + source.y * cosine;
        vertices[index] = makeVertex(
            sprite.centerPixels.x + rotatedX * scale,
            sprite.centerPixels.y - rotatedY * scale,
            source.u,
            source.v,
            bafx::fx::ColorF{
                mappedColor.r,
                mappedColor.g,
                mappedColor.b,
                sprite.color.a},
            sprite.artisticIntensity,
            sprite.dissolveThreshold,
            sprite.contributesBloom,
            1.0F,
            globalOpacity);
    }
    return vertices;
}

[[nodiscard]] std::vector<SpriteVertex> makeTrailVertices(
    const std::span<const bafx::fx::TrailPoint> trail,
    const float trailWidthPixels,
    const float opacity,
    const float globalOpacity,
    const bafx::core::RelativeOklchTheme& theme)
{
    if (trail.size() < 2U || trailWidthPixels <= 0.0F)
    {
        return {};
    }

    std::vector<bafx::core::UnityTrailPoint> points;
    points.reserve(trail.size());
    for (const bafx::fx::TrailPoint& point : trail)
    {
        points.push_back(bafx::core::UnityTrailPoint{
            point.positionPixels.x,
            point.positionPixels.y});
    }

    const bafx::core::UnityTrailMesh mesh = bafx::core::makeUnityTrailMesh(
        points,
        trailWidthPixels);
    std::vector<SpriteVertex> vertices;
    vertices.reserve(mesh.vertices.size());
    for (const bafx::core::UnityTrailVertex& vertex : mesh.vertices)
    {
        const bafx::core::Float3 trailColor =
            bafx::core::evaluateUnityTrailColor(vertex.progress);
        const bafx::core::Float3 mappedColor =
            bafx::core::applyRelativeOklchTheme(trailColor, theme);
        vertices.push_back(makeVertex(
            vertex.x,
            vertex.y,
            1.0F - vertex.progress,
            vertex.transverse,
            bafx::fx::ColorF{
                mappedColor.r,
                mappedColor.g,
                mappedColor.b,
                opacity},
            trailArtisticIntensity,
            0.0F,
            true,
            evaluateTrailLongitudinalCoverage(vertex.progress),
            globalOpacity));
    }
    return vertices;
}

}

struct FxGpuRenderer::Implementation
{
    enum class BloomExecutionPath : std::uint8_t
    {
        Primary,
        RecordingRebuild
    };

    enum class BloomGpuStage : std::uint8_t
    {
        Prefilter,
        Pyramid,
        FinalComposite
    };

    struct BloomTargetRoiState
    {
        ID3D11Texture2D* resource{nullptr};
        std::optional<D3D11_RECT> previousWrittenRect{};
        std::uint64_t lastFullScreenFrameSerial{0U};
        BloomExecutionPath lastWriter{BloomExecutionPath::Primary};
        bool initialized{false};
        bool fullScreenWritten{false};
    };

    [[nodiscard]] static GpuTimestampCheckpoint bloomCheckpoint(
        const BloomExecutionPath path,
        const BloomGpuStage stage) noexcept
    {
        if (path == BloomExecutionPath::Primary)
        {
            switch (stage)
            {
            case BloomGpuStage::Prefilter:
                return GpuTimestampCheckpoint::PrimaryPrefilterComplete;
            case BloomGpuStage::Pyramid:
                return GpuTimestampCheckpoint::PrimaryPyramidComplete;
            case BloomGpuStage::FinalComposite:
                return GpuTimestampCheckpoint::PrimaryFinalCompositeComplete;
            }
        }

        switch (stage)
        {
        case BloomGpuStage::Prefilter:
            return GpuTimestampCheckpoint::RecordingRebuildPrefilterComplete;
        case BloomGpuStage::Pyramid:
            return GpuTimestampCheckpoint::RecordingRebuildPyramidComplete;
        case BloomGpuStage::FinalComposite:
            return GpuTimestampCheckpoint::RecordingRebuildFinalCompositeComplete;
        }
        return GpuTimestampCheckpoint::RecordingRebuildFinalCompositeComplete;
    }

    static void recordBloomCheckpoint(
        GpuTimestampProfiler* const profiler,
        const BloomExecutionPath path,
        const BloomGpuStage stage,
        bool& failure) noexcept
    {
        if (profiler == nullptr)
        {
            return;
        }
        if (profiler->checkpoint(bloomCheckpoint(path, stage))
            != GpuTimestampCheckpointStatus::Recorded)
        {
            failure = true;
        }
    }

    static void skipBloomCheckpoint(
        GpuTimestampProfiler* const profiler,
        const BloomExecutionPath path,
        const BloomGpuStage stage,
        bool& failure) noexcept
    {
        if (profiler == nullptr)
        {
            return;
        }
        if (profiler->skipCheckpoint(bloomCheckpoint(path, stage))
            != GpuTimestampCheckpointStatus::Skipped)
        {
            failure = true;
        }
    }

    static void skipBloomPath(
        GpuTimestampProfiler* const profiler,
        const BloomExecutionPath path,
        bool& failure) noexcept
    {
        skipBloomCheckpoint(profiler, path, BloomGpuStage::Prefilter, failure);
        skipBloomCheckpoint(profiler, path, BloomGpuStage::Pyramid, failure);
        skipBloomCheckpoint(
            profiler,
            path,
            BloomGpuStage::FinalComposite,
            failure);
    }

    Implementation(
        ID3D11Device* sourceDevice,
        ID3D11DeviceContext* sourceContext,
        const WindowSize initialSize,
        const FxBloomSettings initialBloomSettings,
        const CompositionOutputMapping initialOutputMapping,
        const FxGpuRendererFeaturePolicy featurePolicy)
        : device(sourceDevice)
        , context(sourceContext)
        , size(initialSize)
        , bloomSettings(initialBloomSettings)
        , outputMapping(initialOutputMapping)
    {
        if (!hasValidBloomSettings(bloomSettings))
        {
            throw std::invalid_argument("FX Bloom settings are outside the supported range");
        }
        if (outputMapping.intensitySemantics
            != bafx::core::IntensitySemantics::ArtisticRelative)
        {
            throw std::invalid_argument(
                "FX output mapping requires ArtisticRelative intensity");
        }
        createPipeline();
        createTextures();
        createTargets();
        // Context1 can exist on a D3D11.1 runtime while the driver still
        // reports ClearView unsupported. Keep both checks fail-closed because
        // ClearView has no HRESULT with which to repair stale ROI contents.
        if (featurePolicy.allowActiveFxRoiClearView)
        {
            ComPtr<ID3D11DeviceContext1> candidateContext;
            if (SUCCEEDED(context.As(&candidateContext))
                && supportsActiveFxRoiClearView(
                    device.Get(),
                    featurePolicy.activeFxRoiClearViewCapabilityOverride))
            {
                context1 = std::move(candidateContext);
            }
        }
    }

    void createPipeline()
    {
        const ComPtr<ID3DBlob> vertexByteCode = compileShader(
            fxMaterialsShaderSource,
            "SpriteVertex",
            "vs_5_0");
        throwIfFailed(
            device->CreateVertexShader(
                vertexByteCode->GetBufferPointer(),
                vertexByteCode->GetBufferSize(),
                nullptr,
                &vertexShader),
            "ID3D11Device::CreateVertexShader(FX)");

        constexpr std::array inputElements{
            D3D11_INPUT_ELEMENT_DESC{
                "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0,
                static_cast<UINT>(offsetof(SpriteVertex, position)),
                D3D11_INPUT_PER_VERTEX_DATA, 0},
            D3D11_INPUT_ELEMENT_DESC{
                "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0,
                static_cast<UINT>(offsetof(SpriteVertex, uv)),
                D3D11_INPUT_PER_VERTEX_DATA, 0},
            D3D11_INPUT_ELEMENT_DESC{
                "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,
                static_cast<UINT>(offsetof(SpriteVertex, color)),
                D3D11_INPUT_PER_VERTEX_DATA, 0},
            D3D11_INPUT_ELEMENT_DESC{
                "TEXCOORD", 1, DXGI_FORMAT_R32_FLOAT, 0,
                static_cast<UINT>(offsetof(SpriteVertex, intensity)),
                D3D11_INPUT_PER_VERTEX_DATA, 0},
            D3D11_INPUT_ELEMENT_DESC{
                "TEXCOORD", 2, DXGI_FORMAT_R32_FLOAT, 0,
                static_cast<UINT>(offsetof(SpriteVertex, dissolveThreshold)),
                D3D11_INPUT_PER_VERTEX_DATA, 0},
            D3D11_INPUT_ELEMENT_DESC{
                "TEXCOORD", 3, DXGI_FORMAT_R32_FLOAT, 0,
                static_cast<UINT>(offsetof(SpriteVertex, bloomEnabled)),
                D3D11_INPUT_PER_VERTEX_DATA, 0},
            D3D11_INPUT_ELEMENT_DESC{
                "TEXCOORD", 4, DXGI_FORMAT_R32_FLOAT, 0,
                static_cast<UINT>(offsetof(SpriteVertex, coverageFactor)),
                D3D11_INPUT_PER_VERTEX_DATA, 0},
            D3D11_INPUT_ELEMENT_DESC{
                "TEXCOORD", 5, DXGI_FORMAT_R32_FLOAT, 0,
                static_cast<UINT>(offsetof(SpriteVertex, globalOpacity)),
                D3D11_INPUT_PER_VERTEX_DATA, 0}};
        throwIfFailed(
            device->CreateInputLayout(
                inputElements.data(),
                static_cast<UINT>(inputElements.size()),
                vertexByteCode->GetBufferPointer(),
                vertexByteCode->GetBufferSize(),
                &inputLayout),
            "ID3D11Device::CreateInputLayout(FX)");

        createPixelShader("CrossPixel", crossPixelShader);
        createPixelShader("DissolvePixel", dissolvePixelShader);
        createPixelShader("AdditivePixel", additivePixelShader);
        createPixelShader("TrailPixel", trailPixelShader);
        createBloomPipeline();

        D3D11_BUFFER_DESC constantDescription{};
        constantDescription.ByteWidth = sizeof(ViewportConstants);
        constantDescription.Usage = D3D11_USAGE_DYNAMIC;
        constantDescription.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        constantDescription.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        throwIfFailed(
            device->CreateBuffer(&constantDescription, nullptr, &viewportBuffer),
            "ID3D11Device::CreateBuffer(FX viewport)");

        createVertexBuffer(initialVertexCapacity);

        createMaterialSampler(D3D11_TEXTURE_ADDRESS_CLAMP, clampSampler);
        createMaterialSampler(D3D11_TEXTURE_ADDRESS_WRAP, repeatSampler);

        D3D11_RASTERIZER_DESC rasterizerDescription{};
        rasterizerDescription.FillMode = D3D11_FILL_SOLID;
        rasterizerDescription.CullMode = D3D11_CULL_BACK;
        rasterizerDescription.FrontCounterClockwise = FALSE;
        rasterizerDescription.DepthClipEnable = TRUE;
        throwIfFailed(
            device->CreateRasterizerState(&rasterizerDescription, &rasterizerState),
            "ID3D11Device::CreateRasterizerState(FX)");

        D3D11_DEPTH_STENCIL_DESC depthDescription{};
        depthDescription.DepthEnable = FALSE;
        depthDescription.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        depthDescription.DepthFunc = D3D11_COMPARISON_ALWAYS;
        throwIfFailed(
            device->CreateDepthStencilState(&depthDescription, &depthState),
            "ID3D11Device::CreateDepthStencilState(FX)");

        D3D11_BLEND_DESC crossDescription{};
        crossDescription.IndependentBlendEnable = TRUE;
        crossDescription.RenderTarget[0] = sourceOverBlendTarget();
        crossDescription.RenderTarget[1] = sourceOverBlendTarget();
        crossDescription.RenderTarget[2] = coverageUnionBlendTarget();
        crossDescription.RenderTarget[3] = sourceOverBlendTarget();
        throwIfFailed(
            device->CreateBlendState(&crossDescription, &crossBlendState),
            "ID3D11Device::CreateBlendState(Cross)");

        D3D11_BLEND_DESC emissionDescription{};
        emissionDescription.IndependentBlendEnable = TRUE;
        emissionDescription.RenderTarget[0] = additiveBlendTarget();
        emissionDescription.RenderTarget[1] = additiveBlendTarget();
        emissionDescription.RenderTarget[2] = additiveBlendTarget();
        emissionDescription.RenderTarget[3] = additiveBlendTarget();
        throwIfFailed(
            device->CreateBlendState(&emissionDescription, &emissionBlendState),
            "ID3D11Device::CreateBlendState(emission)");
    }

    void createPixelShader(
        const char* entryPoint,
        ComPtr<ID3D11PixelShader>& output)
    {
        const ComPtr<ID3DBlob> byteCode = compileShader(
            fxMaterialsShaderSource,
            entryPoint,
            "ps_5_0");
        throwIfFailed(
            device->CreatePixelShader(
                byteCode->GetBufferPointer(),
                byteCode->GetBufferSize(),
                nullptr,
                &output),
            "ID3D11Device::CreatePixelShader(FX)");
    }

    void createBloomPipeline()
    {
        const ComPtr<ID3DBlob> vertexByteCode = compileShader(
            unityBloomShaderSource(),
            "FullscreenVertex",
            "vs_5_0");
        throwIfFailed(
            device->CreateVertexShader(
                vertexByteCode->GetBufferPointer(),
                vertexByteCode->GetBufferSize(),
                nullptr,
                &fullscreenVertexShader),
            "ID3D11Device::CreateVertexShader(Bloom)");
        createBloomPixelShader("PrefilterPixel", prefilterPixelShader);
        createBloomPixelShader(
            "DifferentialPrefilterPixel",
            differentialPrefilterPixelShader);
        createBloomPixelShader(
            "TemporalBackgroundPixel",
            temporalBackgroundPixelShader);
        createBloomPixelShader("DownsamplePixel", downsamplePixelShader);
        createBloomPixelShader("UpsamplePixel", upsamplePixelShader);
        createBloomPixelShader("BloomResultPixel", bloomResultPixelShader);
        createBloomPixelShader("CompositePixel", compositePixelShader);
        createBloomPixelShader(
            "CaptureCompositePixel",
            captureCompositePixelShader);
        createBloomPixelShader("DesktopCompositePixel", desktopCompositePixelShader);
        createBloomPixelShader(
            "DesktopSdrCompositePixel",
            desktopSdrCompositePixelShader);
        createBloomPixelShader(
            "DesktopCaptureCompositePixel",
            desktopCaptureCompositePixelShader);
        createBloomPixelShader(
            "DesktopCaptureSdrCompositePixel",
            desktopCaptureSdrCompositePixelShader);
        createBloomPixelShader(
            "RecordingCompatibleCompositePixel",
            recordingCompatibleCompositePixelShader);
        createBloomPixelShader(
            "RecordingCompatibleSdrCompositePixel",
            recordingCompatibleSdrCompositePixelShader);
        createBloomPixelShader(
            "LightBackgroundCompositePixel",
            lightBackgroundCompositePixelShader);
        createBloomPixelShader(
            "LightBackgroundSdrCompositePixel",
            lightBackgroundSdrCompositePixelShader);
        createBloomPixelShader(
            "RecordingFxOnlySdrCompositePixel",
            recordingFxOnlySdrCompositePixelShader);
        createBloomPixelShader("CoreCompositePixel", coreCompositePixelShader);
        createBloomPixelShader(
            "CoreRecordingFxOnlySdrCompositePixel",
            coreRecordingFxOnlySdrCompositePixelShader);

        D3D11_BUFFER_DESC constantDescription{};
        constantDescription.ByteWidth = sizeof(BloomConstants);
        constantDescription.Usage = D3D11_USAGE_DYNAMIC;
        constantDescription.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        constantDescription.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        throwIfFailed(
            device->CreateBuffer(&constantDescription, nullptr, &bloomConstantsBuffer),
            "ID3D11Device::CreateBuffer(Bloom constants)");

        constantDescription.ByteWidth = sizeof(BloomResolveRoiConstants);
        throwIfFailed(
            device->CreateBuffer(
                &constantDescription,
                nullptr,
                &bloomResolveRoiConstantsBuffer),
            "ID3D11Device::CreateBuffer(Bloom resolve ROI constants)");

        D3D11_RASTERIZER_DESC rasterizerDescription{};
        rasterizerDescription.FillMode = D3D11_FILL_SOLID;
        rasterizerDescription.CullMode = D3D11_CULL_NONE;
        rasterizerDescription.DepthClipEnable = TRUE;
        throwIfFailed(
            device->CreateRasterizerState(
                &rasterizerDescription,
                &fullscreenRasterizerState),
            "ID3D11Device::CreateRasterizerState(Bloom)");
        rasterizerDescription.ScissorEnable = TRUE;
        throwIfFailed(
            device->CreateRasterizerState(
                &rasterizerDescription,
                &fullscreenScissorRasterizerState),
            "ID3D11Device::CreateRasterizerState(Bloom ROI)");

        D3D11_BLEND_DESC blendDescription{};
        blendDescription.RenderTarget[0].BlendEnable = FALSE;
        blendDescription.RenderTarget[0].RenderTargetWriteMask =
            D3D11_COLOR_WRITE_ENABLE_ALL;
        throwIfFailed(
            device->CreateBlendState(&blendDescription, &overwriteBlendState),
            "ID3D11Device::CreateBlendState(Bloom overwrite)");
    }

    void createBloomPixelShader(
        const char* entryPoint,
        ComPtr<ID3D11PixelShader>& output)
    {
        const ComPtr<ID3DBlob> byteCode = compileShader(
            unityBloomShaderSource(),
            entryPoint,
            "ps_5_0");
        throwIfFailed(
            device->CreatePixelShader(
                byteCode->GetBufferPointer(),
                byteCode->GetBufferSize(),
                nullptr,
                &output),
            "ID3D11Device::CreatePixelShader(Bloom)");
    }

    void createMaterialSampler(
        const D3D11_TEXTURE_ADDRESS_MODE addressMode,
        ComPtr<ID3D11SamplerState>& output)
    {
        D3D11_SAMPLER_DESC description{};
        description.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        description.AddressU = addressMode;
        description.AddressV = addressMode;
        description.AddressW = addressMode;
        description.MinLOD = 0.0F;
        description.MaxLOD = D3D11_FLOAT32_MAX;
        throwIfFailed(
            device->CreateSamplerState(&description, &output),
            "ID3D11Device::CreateSamplerState(FX material)");
    }

    void createVertexBuffer(const std::size_t capacity)
    {
        if (capacity > std::numeric_limits<UINT>::max() / sizeof(SpriteVertex))
        {
            throw std::overflow_error("FX vertex buffer is too large");
        }
        D3D11_BUFFER_DESC description{};
        description.ByteWidth = static_cast<UINT>(capacity * sizeof(SpriteVertex));
        description.Usage = D3D11_USAGE_DYNAMIC;
        description.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        throwIfFailed(
            device->CreateBuffer(&description, nullptr, &vertexBuffer),
            "ID3D11Device::CreateBuffer(FX vertices)");
        vertexCapacity = capacity;
    }

    void createTextures()
    {
        circleTexture = loadPackedFxTexture(
            device.Get(),
            PackedFxTextureId::CenterDisk);
        ringTexture = loadPackedFxTexture(
            device.Get(),
            PackedFxTextureId::DissolveRing);
        triangleTexture = loadPackedFxTexture(
            device.Get(),
            PackedFxTextureId::TriangleAtlas);
        trailTexture = loadPackedFxTexture(
            device.Get(),
            PackedFxTextureId::Trail);
    }

    void updateBloomPlan()
    {
        if (size.width > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())
            || size.height > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()))
        {
            throw std::overflow_error("Bloom source extent exceeds the planner range");
        }
        const bafx::core::UnityBloomPlanResult planResult =
            bafx::core::planUnityBloom(bafx::core::BloomExtent{
                static_cast<std::int32_t>(size.width),
                static_cast<std::int32_t>(size.height)},
                bafx::core::UnityBloomSettings{
                    bloomSettings.diffusion,
                    0.0F,
                    bloomSettings.intensity});
        if (planResult.status != bafx::core::UnityBloomStatus::Ok)
        {
            throw std::runtime_error("Unity Bloom planner rejected the swap-chain extent");
        }
        bloomPlan = planResult.plan;
    }

    void createBloomTargets()
    {
        bloomDownTargets.clear();
        bloomUpTargets.clear();
        bloomDownTargets.reserve(bloomPlan.mipCount);
        if (bloomPlan.mipCount > 1U)
        {
            bloomUpTargets.reserve(static_cast<std::size_t>(bloomPlan.mipCount) - 1U);
        }
        for (std::size_t index = 0U; index < bloomPlan.mipCount; ++index)
        {
            const bafx::core::BloomExtent extent = bloomPlan.mipChain[index];
            const WindowSize targetSize{
                static_cast<std::uint32_t>(extent.width),
                static_cast<std::uint32_t>(extent.height)};
            bloomDownTargets.push_back(createColorTarget(device.Get(), targetSize));
            if (index + 1U < bloomPlan.mipCount)
            {
                bloomUpTargets.push_back(createColorTarget(device.Get(), targetSize));
            }
        }
        initializeBloomTargetRoiStates();
    }

    void initializeBloomTargetRoiStates()
    {
        bloomDownTargetStates.clear();
        bloomUpTargetStates.clear();
        bloomDownTargetStates.reserve(bloomDownTargets.size());
        bloomUpTargetStates.reserve(bloomUpTargets.size());
        for (const ColorTarget& target : bloomDownTargets)
        {
            BloomTargetRoiState state{};
            state.resource = target.texture.Get();
            bloomDownTargetStates.push_back(state);
        }
        for (const ColorTarget& target : bloomUpTargets)
        {
            BloomTargetRoiState state{};
            state.resource = target.texture.Get();
            bloomUpTargetStates.push_back(state);
        }
        primaryRoiActive = false;
        recordingRebuildRoiActive = false;
    }

    void resetBloomTargetRoiStates() noexcept
    {
        for (std::size_t index = 0U;
             index < bloomDownTargetStates.size();
             ++index)
        {
            bloomDownTargetStates[index] = BloomTargetRoiState{};
            if (index < bloomDownTargets.size())
            {
                bloomDownTargetStates[index].resource =
                    bloomDownTargets[index].texture.Get();
            }
        }
        for (std::size_t index = 0U;
             index < bloomUpTargetStates.size();
             ++index)
        {
            bloomUpTargetStates[index] = BloomTargetRoiState{};
            if (index < bloomUpTargets.size())
            {
                bloomUpTargetStates[index].resource =
                    bloomUpTargets[index].texture.Get();
            }
        }
        primaryRoiActive = false;
        recordingRebuildRoiActive = false;
    }

    void clearBloomTargetRoiStates() noexcept
    {
        bloomDownTargetStates.clear();
        bloomUpTargetStates.clear();
        primaryRoiActive = false;
        recordingRebuildRoiActive = false;
    }

    void createTargets()
    {
        directTarget = createColorTarget(device.Get(), size);
        crossTarget = createColorTarget(device.Get(), size);
        bloomSeedTarget = createColorTarget(device.Get(), size);
        occlusionTarget = createColorTarget(
            device.Get(),
            size,
            DXGI_FORMAT_R16G16_FLOAT);
        updateBloomPlan();
        createBloomTargets();
    }

    void unbindFrameResources()
    {
        context->OMSetRenderTargets(0, nullptr, nullptr);
        constexpr std::array<ID3D11ShaderResourceView*, 5> noResources{
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            nullptr};
        context->PSSetShaderResources(
            0,
            static_cast<UINT>(noResources.size()),
            noResources.data());
    }

    void releaseSizeDependentTargets()
    {
        unbindFrameResources();
        clearBloomTargetRoiStates();
        directTarget = {};
        crossTarget = {};
        bloomSeedTarget = {};
        bloomResultTarget = {};
        occlusionTarget = {};
        bloomDownTargets.clear();
        bloomUpTargets.clear();
    }

    void resize(const WindowSize nextSize)
    {
        if (nextSize.width == 0U || nextSize.height == 0U
            || (nextSize.width == size.width && nextSize.height == size.height))
        {
            return;
        }
        releaseSizeDependentTargets();
        size = nextSize;
        createTargets();
    }

    void setBloomSettings(const FxBloomSettings nextSettings)
    {
        if (!hasValidBloomSettings(nextSettings))
        {
            throw std::invalid_argument("FX Bloom settings are outside the supported range");
        }
        if (sameBloomSettings(bloomSettings, nextSettings))
        {
            return;
        }

        const bool pyramidChanged = bloomSettings.diffusion != nextSettings.diffusion;
        bloomSettings = nextSettings;
        if (!pyramidChanged)
        {
            updateBloomPlan();
            resetBloomTargetRoiStates();
            return;
        }

        // Diffusion changes mip extents, so release just the pyramid before
        // replacing it. Direct/coverage targets remain valid for this size.
        unbindFrameResources();
        clearBloomTargetRoiStates();
        bloomDownTargets.clear();
        bloomUpTargets.clear();
        updateBloomPlan();
        createBloomTargets();
    }

    void setThemeColor(const std::string_view themeColor)
    {
        const std::optional<bafx::core::RelativeOklchTheme> parsed =
            bafx::core::createRelativeOklchTheme(themeColor);
        if (!parsed.has_value())
        {
            throw std::invalid_argument("FX theme color must be #rrggbb");
        }
        theme = *parsed;
    }

    void setOverlayProfile(const FxOverlayProfile nextProfile)
    {
        if (!isValidOverlayProfile(nextProfile))
        {
            throw std::invalid_argument("FX overlay profile is not recognized");
        }
        overlayProfile = nextProfile;
        resetBloomTargetRoiStates();
    }

    [[nodiscard]] ID3D11PixelShader* desktopCompositeShader(
        const bool hasBackground) const noexcept
    {
        // A captured background provides the exact source-over target and must
        // take precedence over any unknown-background approximation.
        if (hasBackground || overlayProfile == FxOverlayProfile::FxOnlyFallback)
        {
            return outputTransferShader(
                desktopCompositePixelShader,
                desktopSdrCompositePixelShader);
        }
        if (overlayProfile == FxOverlayProfile::RecordingCompatible)
        {
            return outputTransferShader(
                recordingCompatibleCompositePixelShader,
                recordingCompatibleSdrCompositePixelShader);
        }
        return outputTransferShader(
            lightBackgroundCompositePixelShader,
            lightBackgroundSdrCompositePixelShader);
    }

    [[nodiscard]] ID3D11PixelShader* outputTransferShader(
        const ComPtr<ID3D11PixelShader>& linearShader,
        const ComPtr<ID3D11PixelShader>& sdrShader) const noexcept
    {
        // Intermediate render targets remain linear FP16. Only the shader that
        // writes the swap-chain target follows its negotiated transfer.
        return outputMapping.mode == CompositionOutputMappingMode::ConservativeSdr
            ? sdrShader.Get()
            : linearShader.Get();
    }

    [[nodiscard]] ID3D11PixelShader* desktopCaptureCompositeShader() const noexcept
    {
        return outputTransferShader(
            desktopCaptureCompositePixelShader,
            desktopCaptureSdrCompositePixelShader);
    }

    void stabilizeBackgroundFrame(
        ID3D11ShaderResourceView* const previous,
        ID3D11ShaderResourceView* const current,
        ID3D11RenderTargetView* const destination)
    {
        if (previous == nullptr || current == nullptr || destination == nullptr)
        {
            throw std::invalid_argument(
                "background stabilization requires three valid views");
        }

        const ComPtr<ID3D11Texture2D> previousTexture =
            textureFromShaderResource(previous);
        const ComPtr<ID3D11Texture2D> currentTexture =
            textureFromShaderResource(current);
        const ComPtr<ID3D11Texture2D> destinationTexture =
            textureFromRenderTarget(destination);
        if (!isCompatibleBackgroundTexture(previousTexture.Get(), size)
            || !isCompatibleBackgroundTexture(currentTexture.Get(), size)
            || !isCompatibleBackgroundTexture(destinationTexture.Get(), size))
        {
            throw std::invalid_argument(
                "background stabilization views have incompatible extents");
        }
        if (destinationTexture.Get() == previousTexture.Get()
            || destinationTexture.Get() == currentTexture.Get())
        {
            // Sampling and rendering the same resource is undefined on D3D11;
            // the caller must provide a ping-pong destination.
            throw std::invalid_argument(
                "background stabilization destination aliases a source");
        }

        const bafx::core::BloomExtent extent{
            static_cast<std::int32_t>(size.width),
            static_cast<std::int32_t>(size.height)};
        drawFullscreen(
            destination,
            extent,
            temporalBackgroundPixelShader.Get(),
            previous,
            current,
            makeBloomConstants(
                extent,
                1.0F,
                0.0F,
                false,
                backgroundReferenceWhiteScale(),
                outputReferenceWhiteScale()));
    }

    void configureFramePipeline()
    {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        throwIfFailed(
            context->Map(
                viewportBuffer.Get(),
                0,
                D3D11_MAP_WRITE_DISCARD,
                0,
                &mapped),
            "ID3D11DeviceContext::Map(FX viewport)");
        const ViewportConstants constants{
            {static_cast<float>(size.width), static_cast<float>(size.height)},
            {0.0F, 0.0F}};
        std::memcpy(mapped.pData, &constants, sizeof(constants));
        context->Unmap(viewportBuffer.Get(), 0);

        const D3D11_VIEWPORT viewport{
            0.0F,
            0.0F,
            static_cast<float>(size.width),
            static_cast<float>(size.height),
            0.0F,
            1.0F};
        context->RSSetViewports(1, &viewport);
        context->RSSetState(rasterizerState.Get());
        context->OMSetDepthStencilState(depthState.Get(), 0);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->IASetInputLayout(inputLayout.Get());
        const UINT stride = sizeof(SpriteVertex);
        const UINT offset = 0;
        ID3D11Buffer* vertex = vertexBuffer.Get();
        context->IASetVertexBuffers(0, 1, &vertex, &stride, &offset);
        context->VSSetShader(vertexShader.Get(), nullptr, 0);
        ID3D11Buffer* viewportConstant = viewportBuffer.Get();
        context->VSSetConstantBuffers(0, 1, &viewportConstant);
    }

    [[nodiscard]] BloomConstants makeBloomConstants(
        const bafx::core::BloomExtent sourceExtent,
        const float sampleScale,
        const float exposureGain,
        const bool backgroundTransportEnabled = false,
        const float backgroundReferenceWhiteScale = 1.0F,
        const float outputReferenceWhiteScale = 1.0F) const noexcept
    {
        BloomConstants constants{};
        constants.sourceTexelSize[0] = 1.0F / static_cast<float>(sourceExtent.width);
        constants.sourceTexelSize[1] = 1.0F / static_cast<float>(sourceExtent.height);
        constants.sampleScale = sampleScale;
        constants.exposureGain = exposureGain;
        const float thresholdLinear =
            bafx::core::unityGammaToLinearChannel(bloomSettings.threshold);
        constants.threshold = thresholdLinear;
        // Unity derives knee from the linear threshold before adding epsilon;
        // using the Gamma value changes the soft threshold for HDR inputs.
        constants.knee = thresholdLinear * bloomSettings.softKnee + 0.00001F;
        constants.clampValue = bafx::core::unityGammaToLinearChannel(
            bloomSettings.clampValue);
        constants.backgroundTransportEnabled = backgroundTransportEnabled
            ? 1.0F
            : 0.0F;
        constants.backgroundReferenceWhiteScale =
            backgroundReferenceWhiteScale;
        constants.outputReferenceWhiteScale = outputReferenceWhiteScale;
        constants.themeCoverageScale = theme.coverageScale;
        return constants;
    }

    [[nodiscard]] float backgroundReferenceWhiteScale() const noexcept
    {
        if (!outputMapping.backgroundReferenceWhiteValid
            || !std::isfinite(outputMapping.backgroundReferenceWhiteNits)
            || outputMapping.backgroundReferenceWhiteNits <= 0.0F)
        {
            return 1.0F;
        }
        return outputMapping.backgroundReferenceWhiteNits
            / scRgbNitsPerUnit;
    }

    [[nodiscard]] float outputReferenceWhiteScale() const noexcept
    {
        if (outputMapping.mode
                != CompositionOutputMappingMode::HdrSceneReferredScRgb
            || !outputMapping.referenceWhiteValid
            || !std::isfinite(outputMapping.referenceWhiteNits)
            || outputMapping.referenceWhiteNits <= 0.0F)
        {
            // SDR and unresolved HDR metadata preserve the Unity reference
            // domain. Only a negotiated HDR white maps relative FX to scRGB.
            return 1.0F;
        }
        return outputMapping.referenceWhiteNits / scRgbNitsPerUnit;
    }

    void drawFullscreen(
        ID3D11RenderTargetView* target,
        const bafx::core::BloomExtent targetExtent,
        ID3D11PixelShader* pixelShader,
        ID3D11ShaderResourceView* source0,
        ID3D11ShaderResourceView* source1,
        const BloomConstants& constants,
        ID3D11ShaderResourceView* source2 = nullptr,
        ID3D11ShaderResourceView* source3 = nullptr,
        ID3D11ShaderResourceView* source4 = nullptr,
        const D3D11_RECT* scissor = nullptr)
    {
        const std::array<ID3D11RenderTargetView*, 1> targets{target};
        drawFullscreenTargets(
            targets,
            targetExtent,
            pixelShader,
            source0,
            source1,
            constants,
            source2,
            source3,
            source4,
            scissor);
    }

    void bindBloomResolveRect(const bafx::core::RectI rect)
    {
        const BloomResolveRoiConstants constants{{
            rect.left,
            rect.top,
            rect.right,
            rect.bottom}};
        D3D11_MAPPED_SUBRESOURCE mapped{};
        throwIfFailed(
            context->Map(
                bloomResolveRoiConstantsBuffer.Get(),
                0,
                D3D11_MAP_WRITE_DISCARD,
                0,
                &mapped),
            "ID3D11DeviceContext::Map(Bloom resolve ROI constants)");
        std::memcpy(mapped.pData, &constants, sizeof(constants));
        context->Unmap(bloomResolveRoiConstantsBuffer.Get(), 0);

        ID3D11Buffer* constantBuffer = bloomResolveRoiConstantsBuffer.Get();
        context->PSSetConstantBuffers(1, 1, &constantBuffer);
    }

    void drawFullscreenTargets(
        const std::span<ID3D11RenderTargetView* const> targets,
        const bafx::core::BloomExtent targetExtent,
        ID3D11PixelShader* pixelShader,
        ID3D11ShaderResourceView* source0,
        ID3D11ShaderResourceView* source1,
        const BloomConstants& constants,
        ID3D11ShaderResourceView* source2 = nullptr,
        ID3D11ShaderResourceView* source3 = nullptr,
        ID3D11ShaderResourceView* source4 = nullptr,
        const D3D11_RECT* scissor = nullptr)
    {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        throwIfFailed(
            context->Map(
                bloomConstantsBuffer.Get(),
                0,
                D3D11_MAP_WRITE_DISCARD,
                0,
                &mapped),
            "ID3D11DeviceContext::Map(Bloom constants)");
        std::memcpy(mapped.pData, &constants, sizeof(constants));
        context->Unmap(bloomConstantsBuffer.Get(), 0);

        const D3D11_VIEWPORT viewport{
            0.0F,
            0.0F,
            static_cast<float>(targetExtent.width),
            static_cast<float>(targetExtent.height),
            0.0F,
            1.0F};
        context->RSSetViewports(1, &viewport);
        context->RSSetState(
            scissor == nullptr
                ? fullscreenRasterizerState.Get()
                : fullscreenScissorRasterizerState.Get());
        if (scissor != nullptr)
        {
            context->RSSetScissorRects(1U, scissor);
        }
        context->OMSetDepthStencilState(depthState.Get(), 0);
        context->OMSetRenderTargets(
            static_cast<UINT>(targets.size()),
            targets.data(),
            nullptr);
        constexpr std::array<float, 4> blendFactor{0.0F, 0.0F, 0.0F, 0.0F};
        context->OMSetBlendState(
            overwriteBlendState.Get(),
            blendFactor.data(),
            0xFFFFFFFFU);
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(fullscreenVertexShader.Get(), nullptr, 0);
        context->PSSetShader(pixelShader, nullptr, 0);
        ID3D11Buffer* constantBuffer = bloomConstantsBuffer.Get();
        context->PSSetConstantBuffers(0, 1, &constantBuffer);
        ID3D11SamplerState* bloomSampler = clampSampler.Get();
        context->PSSetSamplers(0, 1, &bloomSampler);
        std::array<ID3D11ShaderResourceView*, 5> sources{
            source0,
            source1,
            source2,
            source3,
            source4};
        context->PSSetShaderResources(
            0,
            static_cast<UINT>(sources.size()),
            sources.data());
        context->Draw(3, 0);

        constexpr std::array<ID3D11ShaderResourceView*, 5> noResources{
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            nullptr};
        context->PSSetShaderResources(
            0,
            static_cast<UINT>(noResources.size()),
            noResources.data());
    }

    [[nodiscard]] static bool sameRect(
        const bafx::core::RectI left,
        const bafx::core::RectI right) noexcept
    {
        return left.left == right.left
            && left.top == right.top
            && left.right == right.right
            && left.bottom == right.bottom;
    }

    [[nodiscard]] static bool samePixelTotals(
        const bafx::core::UnityBloomPassPixelTotals left,
        const bafx::core::UnityBloomPassPixelTotals right) noexcept
    {
        return left.fullPixels == right.fullPixels
            && left.candidatePixels == right.candidatePixels;
    }

    [[nodiscard]] static bool samePassPlan(
        const bafx::core::UnityBloomPassRoiPlan& left,
        const bafx::core::UnityBloomPassRoiPlan& right) noexcept
    {
        if (left.mipCount != right.mipCount
            || left.basePlan.guardX != right.basePlan.guardX
            || left.basePlan.guardY != right.basePlan.guardY
            || left.basePlan.phasePeriod != right.basePlan.phasePeriod
            || !sameRect(left.basePlan.sourceSupport, right.basePlan.sourceSupport)
            || !sameRect(left.basePlan.bloomOutput, right.basePlan.bloomOutput)
            || !sameRect(left.basePlan.alignedWork, right.basePlan.alignedWork)
            || !sameRect(left.resolveRect, right.resolveRect)
            || !samePixelTotals(left.prefilterPixels, right.prefilterPixels)
            || !samePixelTotals(left.pyramidPixels, right.pyramidPixels)
            || !samePixelTotals(left.resolvePixels, right.resolvePixels)
            || !samePixelTotals(left.totalPixels, right.totalPixels))
        {
            return false;
        }
        for (std::size_t index = 0U; index < left.mipCount; ++index)
        {
            if (!sameRect(left.downRects[index], right.downRects[index]))
            {
                return false;
            }
            if (index + 1U < left.mipCount
                && !sameRect(left.upRects[index], right.upRects[index]))
            {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool validateActiveRoiPlan(
        const FxActiveRoi& activeRoi) const noexcept
    {
        if (size.width == 0U
            || size.height == 0U
            || size.width
                > static_cast<std::uint32_t>(
                    std::numeric_limits<std::int32_t>::max())
            || size.height
                > static_cast<std::uint32_t>(
                    std::numeric_limits<std::int32_t>::max()))
        {
            return false;
        }
        const bafx::core::RectI monitor{
            0,
            0,
            static_cast<std::int32_t>(size.width),
            static_cast<std::int32_t>(size.height)};
        const bafx::core::UnityBloomPassRoiPlanResult expected =
            bafx::core::planUnityBloomPassRoi(
                activeRoi.passPlan.basePlan.sourceSupport,
                monitor,
                bloomPlan);
        return expected.status == bafx::core::RoiStatus::Ok
            && samePassPlan(activeRoi.passPlan, expected.plan);
    }

    [[nodiscard]] static D3D11_RECT toScissor(
        const bafx::core::RectI rect) noexcept
    {
        return D3D11_RECT{rect.left, rect.top, rect.right, rect.bottom};
    }

    [[nodiscard]] static std::uint64_t scissorPixels(
        const D3D11_RECT& rect) noexcept
    {
        if (rect.right <= rect.left || rect.bottom <= rect.top)
        {
            return 0U;
        }
        return static_cast<std::uint64_t>(rect.right - rect.left)
            * static_cast<std::uint64_t>(rect.bottom - rect.top);
    }

    [[nodiscard]] static bool sameScissor(
        const D3D11_RECT& left,
        const D3D11_RECT& right) noexcept
    {
        return left.left == right.left
            && left.top == right.top
            && left.right == right.right
            && left.bottom == right.bottom;
    }

    [[nodiscard]] static bool canSkipBloomTargetClear(
        const BloomTargetRoiState& state,
        const D3D11_RECT& currentRect,
        const BloomExecutionPath path) noexcept
    {
        return state.initialized
            && !state.fullScreenWritten
            && state.previousWrittenRect.has_value()
            && state.lastWriter == path
            && sameScissor(*state.previousWrittenRect, currentRect);
    }

    [[nodiscard]] bool synchronizeBloomTargetRoiStates() noexcept
    {
        if (bloomDownTargetStates.size() != bloomDownTargets.size()
            || bloomUpTargetStates.size() != bloomUpTargets.size())
        {
            return false;
        }

        bool changed = false;
        for (std::size_t index = 0U; index < bloomDownTargets.size(); ++index)
        {
            ID3D11Texture2D* const resource =
                bloomDownTargets[index].texture.Get();
            if (resource == nullptr
                || bloomDownTargets[index].renderTarget == nullptr
                || bloomDownTargets[index].shaderResource == nullptr)
            {
                return false;
            }
            if (bloomDownTargetStates[index].resource != resource)
            {
                bloomDownTargetStates[index] = BloomTargetRoiState{};
                bloomDownTargetStates[index].resource = resource;
                changed = true;
            }
        }
        for (std::size_t index = 0U; index < bloomUpTargets.size(); ++index)
        {
            ID3D11Texture2D* const resource = bloomUpTargets[index].texture.Get();
            if (resource == nullptr
                || bloomUpTargets[index].renderTarget == nullptr
                || bloomUpTargets[index].shaderResource == nullptr)
            {
                return false;
            }
            if (bloomUpTargetStates[index].resource != resource)
            {
                bloomUpTargetStates[index] = BloomTargetRoiState{};
                bloomUpTargetStates[index].resource = resource;
                changed = true;
            }
        }
        if (changed)
        {
            primaryRoiActive = false;
            recordingRebuildRoiActive = false;
        }
        return !changed;
    }

    [[nodiscard]] bool& adaptiveRoiState(
        const BloomExecutionPath path) noexcept
    {
        return path == BloomExecutionPath::Primary
            ? primaryRoiActive
            : recordingRebuildRoiActive;
    }

    [[nodiscard]] static std::uint64_t rectPixels(
        const bafx::core::RectI rect) noexcept
    {
        if (rect.right <= rect.left || rect.bottom <= rect.top)
        {
            return 0U;
        }
        return static_cast<std::uint64_t>(rect.right - rect.left)
            * static_cast<std::uint64_t>(rect.bottom - rect.top);
    }

    [[nodiscard]] static std::uint64_t extentPixels(
        const bafx::core::BloomExtent extent) noexcept
    {
        if (extent.width <= 0 || extent.height <= 0)
        {
            return 0U;
        }
        return static_cast<std::uint64_t>(extent.width)
            * static_cast<std::uint64_t>(extent.height);
    }

    static void addPixels(
        std::uint64_t& destination,
        const std::uint64_t value) noexcept
    {
        destination = value
                > std::numeric_limits<std::uint64_t>::max() - destination
            ? std::numeric_limits<std::uint64_t>::max()
            : destination + value;
    }

    static void updateAggregatePixels(
        FxActiveRoiPassDiagnostics& diagnostics) noexcept
    {
        diagnostics.fullPixels = 0U;
        diagnostics.candidatePixels = 0U;
        diagnostics.drawnPixels = 0U;
        diagnostics.clearedPixels = 0U;
        const std::array<const FxActiveRoiStageDiagnostics*, 4U> stages{
            &diagnostics.stages.prefilter,
            &diagnostics.stages.downsample,
            &diagnostics.stages.upsample,
            &diagnostics.stages.resolve};
        for (const FxActiveRoiStageDiagnostics* const stage : stages)
        {
            addPixels(diagnostics.fullPixels, stage->fullPixels);
            addPixels(diagnostics.candidatePixels, stage->candidatePixels);
            addPixels(diagnostics.drawnPixels, stage->drawnPixels);
            addPixels(diagnostics.clearedPixels, stage->clearedPixels);
        }
    }

    void initializeBloomStageDiagnostics(
        FxActiveRoiPassDiagnostics& diagnostics) const noexcept
    {
        diagnostics.stages.prefilter.fullPixels = extentPixels(
            bloomPlan.mipChain[0]);
        for (std::size_t index = 1U; index < bloomPlan.mipCount; ++index)
        {
            addPixels(
                diagnostics.stages.downsample.fullPixels,
                extentPixels(bloomPlan.mipChain[index]));
        }
        for (std::size_t index = 0U;
             index + 1U < bloomPlan.mipCount;
             ++index)
        {
            addPixels(
                diagnostics.stages.upsample.fullPixels,
                extentPixels(bloomPlan.mipChain[index]));
        }
        diagnostics.stages.resolve.fullPixels =
            static_cast<std::uint64_t>(size.width) * size.height;
        diagnostics.stages.prefilter.drawnPixels =
            diagnostics.stages.prefilter.fullPixels;
        diagnostics.stages.downsample.drawnPixels =
            diagnostics.stages.downsample.fullPixels;
        diagnostics.stages.upsample.drawnPixels =
            diagnostics.stages.upsample.fullPixels;
        diagnostics.stages.resolve.drawnPixels =
            diagnostics.stages.resolve.fullPixels;
        updateAggregatePixels(diagnostics);
    }

    static void applyCandidatePixels(
        FxActiveRoiPassDiagnostics& diagnostics,
        const bafx::core::UnityBloomPassRoiPlan& plan) noexcept
    {
        diagnostics.stages.prefilter.candidatePixels =
            plan.prefilterPixels.candidatePixels;
        for (std::size_t index = 1U; index < plan.mipCount; ++index)
        {
            addPixels(
                diagnostics.stages.downsample.candidatePixels,
                rectPixels(plan.downRects[index]));
        }
        for (std::size_t index = 0U; index + 1U < plan.mipCount; ++index)
        {
            addPixels(
                diagnostics.stages.upsample.candidatePixels,
                rectPixels(plan.upRects[index]));
        }
        diagnostics.stages.resolve.candidatePixels =
            plan.resolvePixels.candidatePixels;
        updateAggregatePixels(diagnostics);
    }

    [[nodiscard]] bool bloomTargetsRequireFullClear() const noexcept
    {
        const auto requiresFullClear = [](const BloomTargetRoiState& state)
        {
            return !state.initialized
                || state.fullScreenWritten
                || !state.previousWrittenRect.has_value();
        };
        return std::any_of(
                   bloomDownTargetStates.begin(),
                   bloomDownTargetStates.end(),
                   requiresFullClear)
            || std::any_of(
                bloomUpTargetStates.begin(),
                bloomUpTargetStates.end(),
                requiresFullClear);
    }

    [[nodiscard]] bool hasSharedFullWrite(
        const BloomExecutionPath path) const noexcept
    {
        const auto isShared = [this, path](const BloomTargetRoiState& state)
        {
            return state.fullScreenWritten
                && state.lastWriter != path
                && state.lastFullScreenFrameSerial == renderFrameSerial;
        };
        return std::any_of(
                   bloomDownTargetStates.begin(),
                   bloomDownTargetStates.end(),
                   isShared)
            || std::any_of(
                bloomUpTargetStates.begin(),
                bloomUpTargetStates.end(),
                isShared);
    }

    void clearBloomTargetsForRoi(
        FxActiveRoiPassDiagnostics& diagnostics,
        const bool fullClear,
        const bafx::core::UnityBloomPassRoiPlan& plan,
        const BloomExecutionPath path)
    {
        constexpr std::array<float, 4> transparent{
            0.0F,
            0.0F,
            0.0F,
            0.0F};
        for (std::size_t index = 0U; index < bloomDownTargets.size(); ++index)
        {
            FxActiveRoiStageDiagnostics& stage = index == 0U
                ? diagnostics.stages.prefilter
                : diagnostics.stages.downsample;
            if (fullClear)
            {
                context->ClearRenderTargetView(
                    bloomDownTargets[index].renderTarget.Get(),
                    transparent.data());
                addPixels(stage.clearedPixels, extentPixels(bloomPlan.mipChain[index]));
            }
            else
            {
                const BloomTargetRoiState& state =
                    bloomDownTargetStates[index];
                const D3D11_RECT current = toScissor(plan.downRects[index]);
                // Every Bloom pass overwrites its entire scissor. The same
                // writer can therefore reuse an unchanged initialized rect
                // without exposing stale pixels outside that rect.
                if (canSkipBloomTargetClear(state, current, path))
                {
                    continue;
                }
                const D3D11_RECT previous =
                    *state.previousWrittenRect;
                context1->ClearView(
                    bloomDownTargets[index].renderTarget.Get(),
                    transparent.data(),
                    &previous,
                    1U);
                addPixels(stage.clearedPixels, scissorPixels(previous));
            }
        }
        for (std::size_t index = 0U; index < bloomUpTargets.size(); ++index)
        {
            if (fullClear)
            {
                context->ClearRenderTargetView(
                    bloomUpTargets[index].renderTarget.Get(),
                    transparent.data());
                addPixels(
                    diagnostics.stages.upsample.clearedPixels,
                    extentPixels(bloomPlan.mipChain[index]));
            }
            else
            {
                const BloomTargetRoiState& state = bloomUpTargetStates[index];
                const D3D11_RECT current = toScissor(plan.upRects[index]);
                if (canSkipBloomTargetClear(state, current, path))
                {
                    continue;
                }
                const D3D11_RECT previous =
                    *state.previousWrittenRect;
                context1->ClearView(
                    bloomUpTargets[index].renderTarget.Get(),
                    transparent.data(),
                    &previous,
                    1U);
                addPixels(
                    diagnostics.stages.upsample.clearedPixels,
                    scissorPixels(previous));
            }
        }
        updateAggregatePixels(diagnostics);
    }

    void markBloomTargetsFullScreen(const BloomExecutionPath path) noexcept
    {
        const auto mark = [this, path](BloomTargetRoiState& state)
        {
            state.initialized = true;
            state.fullScreenWritten = true;
            state.previousWrittenRect.reset();
            state.lastWriter = path;
            state.lastFullScreenFrameSerial = renderFrameSerial;
        };
        std::for_each(
            bloomDownTargetStates.begin(),
            bloomDownTargetStates.end(),
            mark);
        std::for_each(
            bloomUpTargetStates.begin(),
            bloomUpTargetStates.end(),
            mark);
    }

    void markBloomTargetsRoi(
        const bafx::core::UnityBloomPassRoiPlan& plan,
        const BloomExecutionPath path) noexcept
    {
        for (std::size_t index = 0U; index < plan.mipCount; ++index)
        {
            BloomTargetRoiState& state = bloomDownTargetStates[index];
            state.initialized = true;
            state.fullScreenWritten = false;
            state.previousWrittenRect = toScissor(plan.downRects[index]);
            state.lastWriter = path;
        }
        for (std::size_t index = 0U; index + 1U < plan.mipCount; ++index)
        {
            BloomTargetRoiState& state = bloomUpTargetStates[index];
            state.initialized = true;
            state.fullScreenWritten = false;
            state.previousWrittenRect = toScissor(plan.upRects[index]);
            state.lastWriter = path;
        }
    }

    [[nodiscard]] FxActiveRoiPassDiagnostics renderBloom(
        ID3D11RenderTargetView* destination,
        ID3D11PixelShader* finalCompositeShader,
        const std::optional<BackgroundRenderInput> background,
        ID3D11RenderTargetView* bloomResultDestination = nullptr,
        const std::optional<FxActiveRoi> activeRoi = std::nullopt,
        const BloomExecutionPath executionPath = BloomExecutionPath::Primary,
        GpuTimestampProfiler* const gpuTimestampProfiler = nullptr,
        bool* const gpuTimestampCheckpointFailure = nullptr)
    {
        const bafx::core::BloomExtent sourceExtent{
            static_cast<std::int32_t>(size.width),
            static_cast<std::int32_t>(size.height)};
        const bafx::core::BloomExtent firstExtent = bloomPlan.mipChain[0];
        const float backgroundWhiteScale = backgroundReferenceWhiteScale();
        const float outputWhiteScale = outputReferenceWhiteScale();

        FxActiveRoiPassDiagnostics roiDiagnostics{};
        roiDiagnostics.requested = activeRoi.has_value();
        roiDiagnostics.executed = true;
        roiDiagnostics.actualPath = FxActiveRoiActualPath::FullScreen;
        roiDiagnostics.decisionReason = activeRoi.has_value()
            ? FxActiveRoiDecisionReason::RendererFallback
            : FxActiveRoiDecisionReason::Disabled;
        initializeBloomStageDiagnostics(roiDiagnostics);

        bool& adaptiveActive = adaptiveRoiState(executionPath);
        const bool resourcesStable = synchronizeBloomTargetRoiStates();
        const bafx::core::UnityBloomPassRoiPlan* roiPlan = nullptr;
        bool useRoi = false;
        if (!activeRoi.has_value())
        {
            adaptiveActive = false;
        }
        else if (validateActiveRoiPlan(*activeRoi))
        {
            roiPlan = &activeRoi->passPlan;
            applyCandidatePixels(roiDiagnostics, *roiPlan);
            const bafx::core::ActiveFxRoiAdaptiveDecision adaptiveDecision =
                bafx::core::decideActiveFxRoiAdaptivePath(
                    adaptiveActive,
                    roiPlan->totalPixels.candidatePixels,
                    roiPlan->totalPixels.fullPixels);
            if (adaptiveDecision
                != bafx::core::ActiveFxRoiAdaptiveDecision::Apply)
            {
                adaptiveActive = false;
                roiDiagnostics.decisionReason =
                    adaptiveDecision
                        == bafx::core::ActiveFxRoiAdaptiveDecision::AreaTooLarge
                    ? FxActiveRoiDecisionReason::AreaTooLarge
                    : FxActiveRoiDecisionReason::BenefitTooSmall;
            }
            else
            {
                roiDiagnostics.eligible = true;
                if (context1 == nullptr)
                {
                    adaptiveActive = false;
                    roiDiagnostics.actualPath =
                        FxActiveRoiActualPath::Unavailable;
                    roiDiagnostics.decisionReason =
                        FxActiveRoiDecisionReason::Context1Unavailable;
                }
                else if (!resourcesStable)
                {
                    adaptiveActive = false;
                }
                else
                {
                    adaptiveActive = true;
                    useRoi = true;
                }
            }
        }
        else
        {
            adaptiveActive = false;
        }

        std::array<D3D11_RECT, bafx::core::unityBloomMaxMipCount>
            downScissors{};
        std::array<D3D11_RECT, bafx::core::unityBloomMaxMipCount - 1U>
            upScissors{};
        if (useRoi)
        {
            for (std::size_t index = 0U; index < roiPlan->mipCount; ++index)
            {
                downScissors[index] = toScissor(roiPlan->downRects[index]);
                if (index + 1U < roiPlan->mipCount)
                {
                    upScissors[index] = toScissor(roiPlan->upRects[index]);
                }
            }
            const bool fullClear = bloomTargetsRequireFullClear();
            const bool sharedFullWrite = fullClear
                && hasSharedFullWrite(executionPath);
            clearBloomTargetsForRoi(
                roiDiagnostics,
                fullClear,
                *roiPlan,
                executionPath);
            roiDiagnostics.warmup = fullClear;
            roiDiagnostics.actualPath = fullClear
                ? FxActiveRoiActualPath::RoiWarmup
                : FxActiveRoiActualPath::RoiPyramid;
            roiDiagnostics.decisionReason = sharedFullWrite
                ? FxActiveRoiDecisionReason::SharedTargetFullWrite
                : FxActiveRoiDecisionReason::Applied;
            roiDiagnostics.stages.prefilter.drawnPixels =
                roiDiagnostics.stages.prefilter.candidatePixels;
            roiDiagnostics.stages.downsample.drawnPixels =
                roiDiagnostics.stages.downsample.candidatePixels;
            roiDiagnostics.stages.upsample.drawnPixels =
                roiDiagnostics.stages.upsample.candidatePixels;
            // Resolve and final composition remain one full-screen draw.
            roiDiagnostics.stages.resolve.drawnPixels =
                roiDiagnostics.stages.resolve.fullPixels;
            updateAggregatePixels(roiDiagnostics);
        }

        drawFullscreen(
            bloomDownTargets[0].renderTarget.Get(),
            firstExtent,
            background.has_value()
                ? differentialPrefilterPixelShader.Get()
                : prefilterPixelShader.Get(),
            bloomSeedTarget.shaderResource.Get(),
            background.has_value() ? background->shaderResource : nullptr,
            makeBloomConstants(
                sourceExtent,
                bloomPlan.sampleScale,
                0.0F,
                false,
                backgroundWhiteScale,
                outputWhiteScale),
            background.has_value()
                ? occlusionTarget.shaderResource.Get()
                : nullptr,
            nullptr,
            nullptr,
            useRoi ? &downScissors[0] : nullptr);

        bool ignoredCheckpointFailure = false;
        bool& checkpointFailure = gpuTimestampCheckpointFailure != nullptr
            ? *gpuTimestampCheckpointFailure
            : ignoredCheckpointFailure;
        recordBloomCheckpoint(
            gpuTimestampProfiler,
            executionPath,
            BloomGpuStage::Prefilter,
            checkpointFailure);

        for (std::size_t index = 1U; index < bloomPlan.mipCount; ++index)
        {
            drawFullscreen(
                bloomDownTargets[index].renderTarget.Get(),
                bloomPlan.mipChain[index],
                downsamplePixelShader.Get(),
                bloomDownTargets[index - 1U].shaderResource.Get(),
                nullptr,
                makeBloomConstants(
                    bloomPlan.mipChain[index - 1U],
                    bloomPlan.sampleScale,
                    0.0F),
                nullptr,
                nullptr,
                nullptr,
                useRoi ? &downScissors[index] : nullptr);
        }

        ID3D11ShaderResourceView* accumulated =
            bloomDownTargets[static_cast<std::size_t>(bloomPlan.mipCount) - 1U]
                .shaderResource.Get();
        for (std::size_t coarseIndex = bloomPlan.mipCount - 1U;
             coarseIndex > 0U;
             --coarseIndex)
        {
            const std::size_t fineIndex = coarseIndex - 1U;
            drawFullscreen(
                bloomUpTargets[fineIndex].renderTarget.Get(),
                bloomPlan.mipChain[fineIndex],
                upsamplePixelShader.Get(),
                accumulated,
                bloomDownTargets[fineIndex].shaderResource.Get(),
                makeBloomConstants(
                    bloomPlan.mipChain[coarseIndex],
                    bloomPlan.sampleScale,
                    0.0F),
                nullptr,
                nullptr,
                nullptr,
                useRoi ? &upScissors[fineIndex] : nullptr);
            accumulated = bloomUpTargets[fineIndex].shaderResource.Get();
        }

        if (useRoi)
        {
            markBloomTargetsRoi(*roiPlan, executionPath);
        }
        else
        {
            markBloomTargetsFullScreen(executionPath);
        }

        recordBloomCheckpoint(
            gpuTimestampProfiler,
            executionPath,
            BloomGpuStage::Pyramid,
            checkpointFailure);

        const BloomConstants finalConstants = makeBloomConstants(
            firstExtent,
            bloomPlan.sampleScale,
            bloomPlan.exposureGain,
            background.has_value(),
            backgroundWhiteScale,
            outputWhiteScale);
        bindBloomResolveRect(useRoi
                ? roiPlan->resolveRect
                : bafx::core::RectI{
                    0,
                    0,
                    static_cast<std::int32_t>(size.width),
                    static_cast<std::int32_t>(size.height)});
        if (bloomResultDestination == nullptr)
        {
            drawFullscreen(
                destination,
                sourceExtent,
                finalCompositeShader,
                directTarget.shaderResource.Get(),
                accumulated,
                finalConstants,
                occlusionTarget.shaderResource.Get(),
                background.has_value() ? background->shaderResource : nullptr,
                crossTarget.shaderResource.Get());
            if (executionPath == BloomExecutionPath::Primary)
            {
                recordBloomCheckpoint(
                    gpuTimestampProfiler,
                    executionPath,
                    BloomGpuStage::FinalComposite,
                    checkpointFailure);
            }
            return roiDiagnostics;
        }

        const std::array<ID3D11RenderTargetView*, 2> targets{
            destination,
            bloomResultDestination};
        drawFullscreenTargets(
            targets,
            sourceExtent,
            finalCompositeShader,
            directTarget.shaderResource.Get(),
            accumulated,
            finalConstants,
            occlusionTarget.shaderResource.Get(),
            background.has_value() ? background->shaderResource : nullptr,
            crossTarget.shaderResource.Get());
        if (executionPath == BloomExecutionPath::Primary)
        {
            recordBloomCheckpoint(
                gpuTimestampProfiler,
                executionPath,
                BloomGpuStage::FinalComposite,
                checkpointFailure);
        }
        return roiDiagnostics;
    }

    void renderWithoutBloom(
        ID3D11RenderTargetView* destination,
        ID3D11PixelShader* finalCompositeShader,
        const std::optional<BackgroundRenderInput> background,
        ID3D11RenderTargetView* bloomResultDestination = nullptr)
    {
        const bafx::core::BloomExtent sourceExtent{
            static_cast<std::int32_t>(size.width),
            static_cast<std::int32_t>(size.height)};
        const BloomConstants constants = makeBloomConstants(
            sourceExtent,
            1.0F,
            0.0F,
            background.has_value(),
            backgroundReferenceWhiteScale(),
            outputReferenceWhiteScale());
        bindBloomResolveRect(bafx::core::RectI{
            0,
            0,
            static_cast<std::int32_t>(size.width),
            static_cast<std::int32_t>(size.height)});
        if (bloomResultDestination == nullptr)
        {
            drawFullscreen(
                destination,
                sourceExtent,
                finalCompositeShader,
                directTarget.shaderResource.Get(),
                nullptr,
                constants,
                occlusionTarget.shaderResource.Get(),
                background.has_value() ? background->shaderResource : nullptr,
                crossTarget.shaderResource.Get());
            return;
        }

        const std::array<ID3D11RenderTargetView*, 2> targets{
            destination,
            bloomResultDestination};
        drawFullscreenTargets(
            targets,
            sourceExtent,
            finalCompositeShader,
            directTarget.shaderResource.Get(),
            nullptr,
            constants,
            occlusionTarget.shaderResource.Get(),
            background.has_value() ? background->shaderResource : nullptr,
            crossTarget.shaderResource.Get());
    }

    void renderRecordingComposite(ID3D11RenderTargetView* const destination)
    {
        const bafx::core::BloomExtent sourceExtent{
            static_cast<std::int32_t>(size.width),
            static_cast<std::int32_t>(size.height)};
        drawFullscreen(
            destination,
            sourceExtent,
            recordingFxOnlySdrCompositePixelShader.Get(),
            directTarget.shaderResource.Get(),
            bloomResultTarget.shaderResource.Get(),
            makeBloomConstants(sourceExtent, 1.0F, 1.0F),
            crossTarget.shaderResource.Get());
    }

    void drawVertices(
        const std::span<const SpriteVertex> vertices,
        ID3D11ShaderResourceView* texture,
        ID3D11SamplerState* materialSampler,
        ID3D11PixelShader* pixelShader,
        ID3D11BlendState* blendState)
    {
        if (vertices.empty())
        {
            return;
        }
        if (vertices.size() > vertexCapacity)
        {
            const std::size_t expanded = std::max(vertices.size(), vertexCapacity * 2U);
            createVertexBuffer(expanded);
            const UINT stride = sizeof(SpriteVertex);
            const UINT offset = 0;
            ID3D11Buffer* vertex = vertexBuffer.Get();
            context->IASetVertexBuffers(0, 1, &vertex, &stride, &offset);
        }

        D3D11_MAPPED_SUBRESOURCE mapped{};
        throwIfFailed(
            context->Map(vertexBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped),
            "ID3D11DeviceContext::Map(FX vertices)");
        std::memcpy(mapped.pData, vertices.data(), vertices.size_bytes());
        context->Unmap(vertexBuffer.Get(), 0);

        constexpr std::array<float, 4> blendFactor{0.0F, 0.0F, 0.0F, 0.0F};
        context->OMSetBlendState(blendState, blendFactor.data(), 0xFFFFFFFFU);
        context->PSSetShader(pixelShader, nullptr, 0);
        context->PSSetShaderResources(0, 1, &texture);
        context->PSSetSamplers(0, 1, &materialSampler);
        context->Draw(static_cast<UINT>(vertices.size()), 0);
    }

    void drawSprite(
        const bafx::fx::Sprite& sprite,
        const float globalOpacity)
    {
        switch (sprite.kind)
        {
        case bafx::fx::SpriteKind::CenterDisk:
        {
            const std::array<SpriteVertex, 6> vertices = makeSpriteVertices(
                sprite,
                globalOpacity,
                theme);
            drawVertices(
                vertices,
                circleTexture.Get(),
                repeatSampler.Get(),
                crossPixelShader.Get(),
                crossBlendState.Get());
            break;
        }

        case bafx::fx::SpriteKind::DissolveRing:
        {
            const auto vertices = makeRingVertices(sprite, globalOpacity, theme);
            drawVertices(
                vertices,
                ringTexture.Get(),
                clampSampler.Get(),
                dissolvePixelShader.Get(),
                emissionBlendState.Get());
            break;
        }

        case bafx::fx::SpriteKind::Triangle:
        {
            const std::array<SpriteVertex, 6> vertices = makeSpriteVertices(
                sprite,
                globalOpacity,
                theme);
            drawVertices(
                vertices,
                triangleTexture.Get(),
                clampSampler.Get(),
                additivePixelShader.Get(),
                emissionBlendState.Get());
            break;
        }
        }
    }

    FxRenderCpuDiagnostics render(
        const bafx::fx::FrameSnapshot& snapshot,
        ID3D11RenderTargetView* destination,
        ID3D11PixelShader* finalCompositeShader,
        std::optional<BackgroundRenderInput> background = std::nullopt,
        ID3D11RenderTargetView* bloomResultDestination = nullptr,
        GpuTimestampProfiler* gpuTimestampProfiler = nullptr,
        ID3D11RenderTargetView* recordingDestination = nullptr,
        const std::optional<FxActiveRoi> activeRoi = std::nullopt)
    {
        FxRenderCpuDiagnostics diagnostics{};
        ++renderFrameSerial;
        const auto totalStartedAt = std::chrono::steady_clock::now();
        if (overlayProfile == FxOverlayProfile::Core)
        {
            return renderCore(
                snapshot,
                destination,
                gpuTimestampProfiler,
                recordingDestination);
        }
        if (background.has_value() && background->shaderResource == nullptr)
        {
            background.reset();
        }

        const ComPtr<ID3D11Texture2D> backgroundTexture = background.has_value()
            ? textureFromShaderResource(background->shaderResource)
            : ComPtr<ID3D11Texture2D>{};
        if (background.has_value()
            && !isCompatibleBackgroundTexture(backgroundTexture.Get(), size))
        {
            background.reset();
        }

        constexpr std::array<float, 4> transparent{0.0F, 0.0F, 0.0F, 0.0F};
        if (!hasVisualContent(snapshot))
        {
            // An empty frame ends the current visual batch. The next batch
            // must re-enter through the 50% gate and clear persistent target
            // contents before its first scissored prefilter draw.
            resetBloomTargetRoiStates();
            diagnostics.primaryActiveFxRoi.actualPath =
                FxActiveRoiActualPath::Idle;
            diagnostics.primaryActiveFxRoi.decisionReason =
                FxActiveRoiDecisionReason::NoContent;
            diagnostics.recordingRebuildActiveFxRoi.actualPath =
                FxActiveRoiActualPath::Idle;
            diagnostics.recordingRebuildActiveFxRoi.decisionReason =
                FxActiveRoiDecisionReason::NoContent;
            context->ClearRenderTargetView(destination, transparent.data());
            if (gpuTimestampProfiler != nullptr)
            {
                // Keep the fixed query boundary sequence complete on idle
                // frames; aggregation excludes these non-visual FX stages.
                (void)gpuTimestampProfiler->checkpoint(
                    GpuTimestampCheckpoint::FxMaterialsComplete);
            }
            if (recordingDestination != nullptr)
            {
                context->ClearRenderTargetView(
                    recordingDestination,
                    transparent.data());
            }
            skipBloomPath(
                gpuTimestampProfiler,
                BloomExecutionPath::Primary,
                diagnostics.gpuTimestampCheckpointFailure);
            skipBloomPath(
                gpuTimestampProfiler,
                BloomExecutionPath::RecordingRebuild,
                diagnostics.gpuTimestampCheckpointFailure);
            diagnostics.totalSubmit =
                std::chrono::steady_clock::now() - totalStartedAt;
            return diagnostics;
        }
        diagnostics.visualContent = true;

        // Keep FX emission independent from capture state. The final pass uses
        // the small occlusion target to reconstruct how Cross2 attenuates WGC;
        // additive materials never suppress the captured desktop.
        context->ClearRenderTargetView(
            directTarget.renderTarget.Get(),
            transparent.data());
        context->ClearRenderTargetView(
            crossTarget.renderTarget.Get(),
            transparent.data());
        context->ClearRenderTargetView(
            bloomSeedTarget.renderTarget.Get(),
            transparent.data());
        context->ClearRenderTargetView(
            occlusionTarget.renderTarget.Get(),
            transparent.data());
        std::array<ID3D11RenderTargetView*, 4> targets{
            directTarget.renderTarget.Get(),
            bloomSeedTarget.renderTarget.Get(),
            occlusionTarget.renderTarget.Get(),
            crossTarget.renderTarget.Get()};
        context->OMSetRenderTargets(
            static_cast<UINT>(targets.size()),
            targets.data(),
            nullptr);
        configureFramePipeline();

        std::size_t index = 0U;
        while (index < snapshot.sprites.size()
            && snapshot.sprites[index].renderQueue <= trailRenderQueue)
        {
            drawSprite(snapshot.sprites[index], snapshot.globalOpacity);
            ++index;
        }
        const auto drawTrail = [this](
                                   const std::span<const bafx::fx::TrailPoint> points,
                                   const float widthPixels,
                                   const float opacity,
                                   const float globalOpacity)
        {
            const std::vector<SpriteVertex> trailVertices = makeTrailVertices(
                points,
                widthPixels,
                opacity,
                globalOpacity,
                theme);
            drawVertices(
                trailVertices,
                trailTexture.Get(),
                repeatSampler.Get(),
                trailPixelShader.Get(),
                emissionBlendState.Get());
        };
        if (snapshot.trailStrokes.empty())
        {
            drawTrail(
                snapshot.trail,
                snapshot.trailWidthPixels,
                snapshot.trailOpacity,
                snapshot.globalOpacity);
        }
        else
        {
            for (const bafx::fx::TrailStroke& stroke : snapshot.trailStrokes)
            {
                drawTrail(
                    stroke.points,
                    stroke.widthPixels,
                    stroke.opacity,
                    snapshot.globalOpacity);
            }
        }
        while (index < snapshot.sprites.size())
        {
            drawSprite(snapshot.sprites[index], snapshot.globalOpacity);
            ++index;
        }

        diagnostics.materialsSubmit =
            std::chrono::steady_clock::now() - totalStartedAt;
        if (gpuTimestampProfiler != nullptr)
        {
            (void)gpuTimestampProfiler->checkpoint(
                GpuTimestampCheckpoint::FxMaterialsComplete);
        }
        // Unbind the material MRTs before sampling them through the Bloom chain.
        context->OMSetRenderTargets(0, nullptr, nullptr);
        const auto bloomStartedAt = std::chrono::steady_clock::now();
        if (bloomSettings.enabled)
        {
            diagnostics.primaryActiveFxRoi = renderBloom(
                destination,
                finalCompositeShader,
                background,
                bloomResultDestination,
                background.has_value() ? std::nullopt : activeRoi,
                BloomExecutionPath::Primary,
                gpuTimestampProfiler,
                &diagnostics.gpuTimestampCheckpointFailure);
            if (background.has_value() && activeRoi.has_value())
            {
                diagnostics.primaryActiveFxRoi.requested = true;
                diagnostics.primaryActiveFxRoi.eligible = false;
                diagnostics.primaryActiveFxRoi.decisionReason =
                    FxActiveRoiDecisionReason::BackgroundDifferentialBloom;
            }
        }
        else
        {
            renderWithoutBloom(
                destination,
                finalCompositeShader,
                background,
                bloomResultDestination);
            skipBloomCheckpoint(
                gpuTimestampProfiler,
                BloomExecutionPath::Primary,
                BloomGpuStage::Prefilter,
                diagnostics.gpuTimestampCheckpointFailure);
            skipBloomCheckpoint(
                gpuTimestampProfiler,
                BloomExecutionPath::Primary,
                BloomGpuStage::Pyramid,
                diagnostics.gpuTimestampCheckpointFailure);
            recordBloomCheckpoint(
                gpuTimestampProfiler,
                BloomExecutionPath::Primary,
                BloomGpuStage::FinalComposite,
                diagnostics.gpuTimestampCheckpointFailure);
        }
        if (recordingDestination != nullptr)
        {
            if (!bloomSettings.enabled)
            {
                // The recording composite samples this persistent target.
                // Clear it on every disabled frame so a hot toggle cannot
                // reuse Bloom from the preceding frame.
                context->ClearRenderTargetView(
                    bloomResultTarget.renderTarget.Get(),
                    transparent.data());
            }
            else if (background.has_value())
            {
                // Differential Bloom belongs only to the desktop path. Rebuild
                // pure FX Bloom so OBS output cannot vary with WGC pixels.
                diagnostics.recordingRebuildActiveFxRoi = renderBloom(
                    bloomResultTarget.renderTarget.Get(),
                    bloomResultPixelShader.Get(),
                    std::nullopt,
                    nullptr,
                    activeRoi,
                    BloomExecutionPath::RecordingRebuild,
                    gpuTimestampProfiler,
                    &diagnostics.gpuTimestampCheckpointFailure);
            }
            renderRecordingComposite(recordingDestination);
            if (bloomSettings.enabled && background.has_value())
            {
                recordBloomCheckpoint(
                    gpuTimestampProfiler,
                    BloomExecutionPath::RecordingRebuild,
                    BloomGpuStage::FinalComposite,
                    diagnostics.gpuTimestampCheckpointFailure);
            }
            else
            {
                skipBloomPath(
                    gpuTimestampProfiler,
                    BloomExecutionPath::RecordingRebuild,
                    diagnostics.gpuTimestampCheckpointFailure);
            }
        }
        else
        {
            skipBloomPath(
                gpuTimestampProfiler,
                BloomExecutionPath::RecordingRebuild,
                diagnostics.gpuTimestampCheckpointFailure);
        }

        const auto isApplied = [](const FxActiveRoiPassDiagnostics& pass)
        {
            return pass.actualPath == FxActiveRoiActualPath::RoiWarmup
                || pass.actualPath == FxActiveRoiActualPath::RoiPrefilter
                || pass.actualPath == FxActiveRoiActualPath::RoiPyramid;
        };
        if (isApplied(diagnostics.primaryActiveFxRoi))
        {
            diagnostics.activeFxRoiApplied = true;
            diagnostics.activeFxRoiPixels =
                diagnostics.primaryActiveFxRoi.drawnPixels;
        }
        else if (isApplied(diagnostics.recordingRebuildActiveFxRoi))
        {
            diagnostics.activeFxRoiApplied = true;
            diagnostics.activeFxRoiPixels =
                diagnostics.recordingRebuildActiveFxRoi.drawnPixels;
        }
        context->OMSetRenderTargets(0, nullptr, nullptr);
        diagnostics.bloomAndCompositeSubmit =
            std::chrono::steady_clock::now() - bloomStartedAt;
        diagnostics.totalSubmit =
            std::chrono::steady_clock::now() - totalStartedAt;
        return diagnostics;
    }

    FxRenderCpuDiagnostics renderCore(
        const bafx::fx::FrameSnapshot& snapshot,
        ID3D11RenderTargetView* destination,
        GpuTimestampProfiler* const gpuTimestampProfiler,
        ID3D11RenderTargetView* const recordingDestination)
    {
        FxRenderCpuDiagnostics diagnostics{};
        const auto startedAt = std::chrono::steady_clock::now();
        constexpr std::array<float, 4> transparent{0.0F, 0.0F, 0.0F, 0.0F};
        if (!hasVisualContent(snapshot))
        {
            context->ClearRenderTargetView(destination, transparent.data());
            if (recordingDestination != nullptr)
            {
                context->ClearRenderTargetView(
                    recordingDestination,
                    transparent.data());
            }
            if (gpuTimestampProfiler != nullptr)
            {
                (void)gpuTimestampProfiler->checkpoint(
                    GpuTimestampCheckpoint::FxMaterialsComplete);
            }
            skipBloomPath(
                gpuTimestampProfiler,
                BloomExecutionPath::Primary,
                diagnostics.gpuTimestampCheckpointFailure);
            skipBloomPath(
                gpuTimestampProfiler,
                BloomExecutionPath::RecordingRebuild,
                diagnostics.gpuTimestampCheckpointFailure);
            diagnostics.totalSubmit = std::chrono::steady_clock::now()
                - startedAt;
            return diagnostics;
        }

        diagnostics.visualContent = true;
        context->ClearRenderTargetView(
            directTarget.renderTarget.Get(),
            transparent.data());
        if (recordingDestination == nullptr)
        {
            const std::array<ID3D11RenderTargetView*, 1> targets{
                directTarget.renderTarget.Get()};
            context->OMSetRenderTargets(
                static_cast<UINT>(targets.size()),
                targets.data(),
                nullptr);
        }
        else
        {
            context->ClearRenderTargetView(
                crossTarget.renderTarget.Get(),
                transparent.data());
            // Core skips Bloom, but Spout still needs Cross2 isolated from
            // additive materials so only real coverage attenuates OBS.
            const std::array<ID3D11RenderTargetView*, 4> targets{
                directTarget.renderTarget.Get(),
                nullptr,
                nullptr,
                crossTarget.renderTarget.Get()};
            context->OMSetRenderTargets(
                static_cast<UINT>(targets.size()),
                targets.data(),
                nullptr);
        }
        configureFramePipeline();
        std::size_t index = 0U;
        while (index < snapshot.sprites.size()
            && snapshot.sprites[index].renderQueue <= trailRenderQueue)
        {
            drawSprite(snapshot.sprites[index], snapshot.globalOpacity);
            ++index;
        }
        const auto drawTrail = [this](
                                   const std::span<const bafx::fx::TrailPoint> points,
                                   const float widthPixels,
                                   const float opacity,
                                   const float globalOpacity)
        {
            const std::vector<SpriteVertex> trailVertices = makeTrailVertices(
                points,
                widthPixels,
                opacity,
                globalOpacity,
                theme);
            drawVertices(
                trailVertices,
                trailTexture.Get(),
                repeatSampler.Get(),
                trailPixelShader.Get(),
                emissionBlendState.Get());
        };
        if (snapshot.trailStrokes.empty())
        {
            drawTrail(
                snapshot.trail,
                snapshot.trailWidthPixels,
                snapshot.trailOpacity,
                snapshot.globalOpacity);
        }
        else
        {
            for (const bafx::fx::TrailStroke& stroke : snapshot.trailStrokes)
            {
                drawTrail(
                    stroke.points,
                    stroke.widthPixels,
                    stroke.opacity,
                    snapshot.globalOpacity);
            }
        }
        while (index < snapshot.sprites.size())
        {
            drawSprite(snapshot.sprites[index], snapshot.globalOpacity);
            ++index;
        }
        context->OMSetRenderTargets(0, nullptr, nullptr);
        diagnostics.materialsSubmit = std::chrono::steady_clock::now()
            - startedAt;
        if (gpuTimestampProfiler != nullptr)
        {
            (void)gpuTimestampProfiler->checkpoint(
                GpuTimestampCheckpoint::FxMaterialsComplete);
        }

        const bafx::core::BloomExtent sourceExtent{
            static_cast<std::int32_t>(size.width),
            static_cast<std::int32_t>(size.height)};
        const auto compositeStartedAt = std::chrono::steady_clock::now();
        drawFullscreen(
            destination,
            sourceExtent,
            coreCompositePixelShader.Get(),
            directTarget.shaderResource.Get(),
            nullptr,
            makeBloomConstants(sourceExtent, 0.0F, 0.0F));
        if (recordingDestination != nullptr)
        {
            drawFullscreen(
                recordingDestination,
                sourceExtent,
                coreRecordingFxOnlySdrCompositePixelShader.Get(),
                directTarget.shaderResource.Get(),
                nullptr,
                makeBloomConstants(sourceExtent, 0.0F, 0.0F),
                crossTarget.shaderResource.Get());
        }
        skipBloomPath(
            gpuTimestampProfiler,
            BloomExecutionPath::Primary,
            diagnostics.gpuTimestampCheckpointFailure);
        skipBloomPath(
            gpuTimestampProfiler,
            BloomExecutionPath::RecordingRebuild,
            diagnostics.gpuTimestampCheckpointFailure);
        context->OMSetRenderTargets(0, nullptr, nullptr);
        diagnostics.bloomAndCompositeSubmit = std::chrono::steady_clock::now()
            - compositeStartedAt;
        diagnostics.totalSubmit = std::chrono::steady_clock::now() - startedAt;
        return diagnostics;
    }

    void ensureBloomResultTarget()
    {
        if (bloomResultTarget.texture == nullptr)
        {
            // Capture diagnostics must not add a full-resolution target or a
            // second output to the desktop's ordinary rendering path.
            bloomResultTarget = createColorTarget(device.Get(), size);
        }
    }

    [[nodiscard]] FxGpuFrameCapture renderAndCapture(
        const bafx::fx::FrameSnapshot& snapshot,
        ID3D11RenderTargetView* destination,
        const std::optional<FxActiveRoi> activeRoi)
    {
        if (overlayProfile == FxOverlayProfile::Core)
        {
            const bool visualContent = hasVisualContent(snapshot);
            renderCore(snapshot, destination, nullptr, nullptr);
            FxGpuFrameCapture capture{};
            const ComPtr<ID3D11Texture2D> destinationTexture =
                textureFromRenderTarget(destination);
            capture.finalOverlay = readbackRgba16FloatTexture(
                context.Get(),
                destinationTexture.Get());
            if (visualContent)
            {
                capture.directSurface = readbackRgba16FloatTexture(
                    context.Get(),
                    directTarget.texture.Get());
            }
            return capture;
        }
        const bool visualContent = hasVisualContent(snapshot);
        if (visualContent)
        {
            ensureBloomResultTarget();
            render(
                snapshot,
                destination,
                captureCompositePixelShader.Get(),
                std::nullopt,
                bloomResultTarget.renderTarget.Get(),
                nullptr,
                nullptr,
                activeRoi);
        }
        else
        {
            render(
                snapshot,
                destination,
                compositePixelShader.Get(),
                std::nullopt,
                nullptr,
                nullptr,
                nullptr,
                activeRoi);
        }

        FxGpuFrameCapture capture{};
        const ComPtr<ID3D11Texture2D> destinationTexture =
            textureFromRenderTarget(destination);
        capture.finalOverlay = readbackRgba16FloatTexture(
            context.Get(),
            destinationTexture.Get());
        if (!visualContent)
        {
            return capture;
        }

        capture.directSurface = readbackRgba16FloatTexture(
            context.Get(),
            directTarget.texture.Get());
        capture.bloomSeed = readbackRgba16FloatTexture(
            context.Get(),
            bloomSeedTarget.texture.Get());
        capture.bloomDown.reserve(bloomDownTargets.size());
        for (const ColorTarget& target : bloomDownTargets)
        {
            capture.bloomDown.push_back(readbackRgba16FloatTexture(
                context.Get(),
                target.texture.Get()));
        }
        capture.bloomUp.reserve(bloomUpTargets.size());
        for (const ColorTarget& target : bloomUpTargets)
        {
            capture.bloomUp.push_back(readbackRgba16FloatTexture(
                context.Get(),
                target.texture.Get()));
        }
        capture.bloomResult = readbackRgba16FloatTexture(
            context.Get(),
            bloomResultTarget.texture.Get());
        capture.intermediateLayersValid = true;
        return capture;
    }

    FxRenderCpuDiagnostics renderDesktop(
        const bafx::fx::FrameSnapshot& snapshot,
        ID3D11RenderTargetView* const destination,
        const std::optional<BackgroundRenderInput> background,
        GpuTimestampProfiler* const gpuTimestampProfiler,
        ID3D11RenderTargetView* const recordingDestination,
        const std::optional<FxActiveRoi> activeRoi)
    {
        if (recordingDestination != nullptr)
        {
            if (overlayProfile != FxOverlayProfile::Core)
            {
                ensureBloomResultTarget();
            }
            return render(
                snapshot,
                destination,
                overlayProfile == FxOverlayProfile::Core
                    ? desktopCompositeShader(background.has_value())
                    : desktopCaptureCompositeShader(),
                background,
                overlayProfile == FxOverlayProfile::Core
                    ? nullptr
                    : bloomResultTarget.renderTarget.Get(),
                gpuTimestampProfiler,
                recordingDestination,
                activeRoi);
        }
        return render(
            snapshot,
            destination,
            desktopCompositeShader(background.has_value()),
            background,
            nullptr,
            gpuTimestampProfiler,
            nullptr,
            activeRoi);
    }

    ComPtr<ID3D11Device> device{};
    ComPtr<ID3D11DeviceContext> context{};
    ComPtr<ID3D11DeviceContext1> context1{};
    WindowSize size{};
    FxBloomSettings bloomSettings{};
    CompositionOutputMapping outputMapping{
        compositionOutputPolicyFor(
            CompositionOutputPreference::PreferLinearScRgb).mapping};
    ColorTarget directTarget{};
    ColorTarget crossTarget{};
    ColorTarget bloomSeedTarget{};
    ColorTarget bloomResultTarget{};
    ColorTarget occlusionTarget{};
    bafx::core::UnityBloomPlan bloomPlan{};
    std::vector<ColorTarget> bloomDownTargets{};
    std::vector<ColorTarget> bloomUpTargets{};
    std::vector<BloomTargetRoiState> bloomDownTargetStates{};
    std::vector<BloomTargetRoiState> bloomUpTargetStates{};
    std::uint64_t renderFrameSerial{0U};
    bool primaryRoiActive{false};
    bool recordingRebuildRoiActive{false};
    ComPtr<ID3D11VertexShader> vertexShader{};
    ComPtr<ID3D11VertexShader> fullscreenVertexShader{};
    ComPtr<ID3D11PixelShader> crossPixelShader{};
    ComPtr<ID3D11PixelShader> dissolvePixelShader{};
    ComPtr<ID3D11PixelShader> additivePixelShader{};
    ComPtr<ID3D11PixelShader> trailPixelShader{};
    ComPtr<ID3D11PixelShader> prefilterPixelShader{};
    ComPtr<ID3D11PixelShader> differentialPrefilterPixelShader{};
    ComPtr<ID3D11PixelShader> temporalBackgroundPixelShader{};
    ComPtr<ID3D11PixelShader> downsamplePixelShader{};
    ComPtr<ID3D11PixelShader> upsamplePixelShader{};
    ComPtr<ID3D11PixelShader> bloomResultPixelShader{};
    ComPtr<ID3D11PixelShader> compositePixelShader{};
    ComPtr<ID3D11PixelShader> captureCompositePixelShader{};
    ComPtr<ID3D11PixelShader> desktopCompositePixelShader{};
    ComPtr<ID3D11PixelShader> desktopSdrCompositePixelShader{};
    ComPtr<ID3D11PixelShader> desktopCaptureCompositePixelShader{};
    ComPtr<ID3D11PixelShader> desktopCaptureSdrCompositePixelShader{};
    ComPtr<ID3D11PixelShader> recordingCompatibleCompositePixelShader{};
    ComPtr<ID3D11PixelShader> recordingCompatibleSdrCompositePixelShader{};
    ComPtr<ID3D11PixelShader> lightBackgroundCompositePixelShader{};
    ComPtr<ID3D11PixelShader> lightBackgroundSdrCompositePixelShader{};
    ComPtr<ID3D11PixelShader> recordingFxOnlySdrCompositePixelShader{};
    ComPtr<ID3D11PixelShader> coreCompositePixelShader{};
    ComPtr<ID3D11PixelShader> coreRecordingFxOnlySdrCompositePixelShader{};
    ComPtr<ID3D11InputLayout> inputLayout{};
    ComPtr<ID3D11Buffer> vertexBuffer{};
    ComPtr<ID3D11Buffer> viewportBuffer{};
    ComPtr<ID3D11Buffer> bloomConstantsBuffer{};
    ComPtr<ID3D11Buffer> bloomResolveRoiConstantsBuffer{};
    ComPtr<ID3D11SamplerState> clampSampler{};
    ComPtr<ID3D11SamplerState> repeatSampler{};
    ComPtr<ID3D11RasterizerState> rasterizerState{};
    ComPtr<ID3D11RasterizerState> fullscreenRasterizerState{};
    ComPtr<ID3D11RasterizerState> fullscreenScissorRasterizerState{};
    ComPtr<ID3D11DepthStencilState> depthState{};
    ComPtr<ID3D11BlendState> crossBlendState{};
    ComPtr<ID3D11BlendState> emissionBlendState{};
    ComPtr<ID3D11BlendState> overwriteBlendState{};
    ComPtr<ID3D11ShaderResourceView> circleTexture{};
    ComPtr<ID3D11ShaderResourceView> ringTexture{};
    ComPtr<ID3D11ShaderResourceView> triangleTexture{};
    ComPtr<ID3D11ShaderResourceView> trailTexture{};
    std::size_t vertexCapacity{0U};
    FxOverlayProfile overlayProfile{FxOverlayProfile::FxOnlyFallback};
    bafx::core::RelativeOklchTheme theme{};
};

FxGpuRenderer::FxGpuRenderer(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    const WindowSize size,
    const FxBloomSettings bloomSettings,
    const CompositionOutputMapping outputMapping,
    const FxGpuRendererFeaturePolicy featurePolicy)
    : implementation_(std::make_unique<Implementation>(
        device,
        context,
        size,
        bloomSettings,
        outputMapping,
        featurePolicy))
{
}

FxGpuRenderer::~FxGpuRenderer() = default;

void FxGpuRenderer::resize(const WindowSize size)
{
    implementation_->resize(size);
}

void FxGpuRenderer::setBloomSettings(const FxBloomSettings settings)
{
    implementation_->setBloomSettings(settings);
}

void FxGpuRenderer::resetActiveFxRoiState() noexcept
{
    implementation_->resetBloomTargetRoiStates();
}

void FxGpuRenderer::setThemeColor(const std::string_view themeColor)
{
    implementation_->setThemeColor(themeColor);
}

void FxGpuRenderer::setOverlayProfile(const FxOverlayProfile profile)
{
    implementation_->setOverlayProfile(profile);
}

void FxGpuRenderer::stabilizeBackgroundFrame(
    ID3D11ShaderResourceView* const previous,
    ID3D11ShaderResourceView* const current,
    ID3D11RenderTargetView* const destination)
{
    implementation_->stabilizeBackgroundFrame(previous, current, destination);
}

FxRenderCpuDiagnostics FxGpuRenderer::render(
    const bafx::fx::FrameSnapshot& snapshot,
    ID3D11RenderTargetView* destination,
    const std::optional<BackgroundRenderInput> background,
    GpuTimestampProfiler* const gpuTimestampProfiler,
    ID3D11RenderTargetView* const recordingDestination,
    const std::optional<FxActiveRoi> activeRoi)
{
    return implementation_->renderDesktop(
        snapshot,
        destination,
        background,
        gpuTimestampProfiler,
        recordingDestination,
        activeRoi);
}

FxGpuFrameCapture FxGpuRenderer::renderAndCapture(
    const bafx::fx::FrameSnapshot& snapshot,
    ID3D11RenderTargetView* destination,
    const std::optional<FxActiveRoi> activeRoi)
{
    return implementation_->renderAndCapture(snapshot, destination, activeRoi);
}

}
