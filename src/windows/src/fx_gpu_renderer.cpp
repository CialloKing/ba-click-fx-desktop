#include "bafx/windows/fx_gpu_renderer.hpp"

#include "bafx/core/unity_bloom.hpp"
#include "bafx/core/unity_ring_mesh.hpp"
#include "bafx/core/unity_trail_mesh.hpp"
#include "bafx/windows/error.hpp"
#include "bafx/windows/gpu_texture_readback.hpp"
#include "embedded_fx_shaders.hpp"
#include "embedded_unity_textures.hpp"
#include "wic_texture_loader.hpp"

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
#include <vector>

namespace bafx::windows
{
namespace
{

using Microsoft::WRL::ComPtr;

constexpr float trailArtisticIntensity = 23.968628F;
constexpr std::int32_t trailRenderQueue = 4499;
constexpr std::size_t initialVertexCapacity = bafx::core::unityRingIndexCount;
constexpr float unityBloomIntensity = 1.7F;
constexpr float minimumBloomDiffusion = 1.0F;
constexpr float maximumBloomDiffusion = 10.0F;
constexpr float maximumBloomIntensityMultiplier = 8.0F;

struct SpriteVertex
{
    float position[2]{};
    float uv[2]{};
    float color[4]{};
    float intensity{1.0F};
    float dissolveThreshold{0.0F};
    float bloomEnabled{0.0F};
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
    float padding{0.0F};
};

struct ColorTarget
{
    ComPtr<ID3D11Texture2D> texture{};
    ComPtr<ID3D11RenderTargetView> renderTarget{};
    ComPtr<ID3D11ShaderResourceView> shaderResource{};
};

[[nodiscard]] bool hasValidBloomSettings(const FxBloomSettings settings) noexcept
{
    return std::isfinite(settings.intensityMultiplier)
        && settings.intensityMultiplier >= 0.0F
        && settings.intensityMultiplier <= maximumBloomIntensityMultiplier
        && std::isfinite(settings.diffusion)
        && settings.diffusion >= minimumBloomDiffusion
        && settings.diffusion <= maximumBloomDiffusion;
}

[[nodiscard]] bool sameBloomSettings(
    const FxBloomSettings left,
    const FxBloomSettings right) noexcept
{
    return left.intensityMultiplier == right.intensityMultiplier
        && left.diffusion == right.diffusion;
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

[[nodiscard]] ColorTarget createColorTarget(
    ID3D11Device* device,
    const WindowSize size)
{
    D3D11_TEXTURE2D_DESC description{};
    description.Width = size.width;
    description.Height = size.height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
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
    return snapshot.active
        || !snapshot.sprites.empty()
        || !snapshot.trail.empty()
        || !snapshot.trailStrokes.empty();
}

[[nodiscard]] SpriteVertex makeVertex(
    const float x,
    const float y,
    const float u,
    const float v,
    const bafx::fx::ColorF color,
    const float intensity,
    const float dissolveThreshold,
    const bool contributesBloom) noexcept
{
    return SpriteVertex{
        {x, y},
        {u, v},
        {color.r, color.g, color.b, color.a},
        intensity,
        dissolveThreshold,
        contributesBloom ? 1.0F : 0.0F};
}

[[nodiscard]] std::array<SpriteVertex, 6> makeSpriteVertices(
    const bafx::fx::Sprite& sprite) noexcept
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

    const auto vertex = [&sprite, &positions, &uvs](const std::size_t index)
    {
        return makeVertex(
            positions[index][0],
            positions[index][1],
            uvs[index][0],
            uvs[index][1],
            sprite.color,
            sprite.artisticIntensity,
            sprite.dissolveThreshold,
            sprite.contributesBloom);
    };
    return {vertex(0), vertex(1), vertex(2), vertex(0), vertex(2), vertex(3)};
}

[[nodiscard]] std::array<SpriteVertex, bafx::core::unityRingIndexCount>
makeRingVertices(const bafx::fx::Sprite& sprite) noexcept
{
    // Cylinder002 is regular, so its exact topology can remain code-generated.
    static const bafx::core::UnityRingMesh mesh = bafx::core::makeUnityRingMesh();
    std::array<SpriteVertex, bafx::core::unityRingIndexCount> vertices{};
    const float scale = sprite.sizePixels
        / (2.0F * bafx::core::unityRingOuterRadius);
    const float cosine = std::cos(sprite.rotationRadians);
    const float sine = std::sin(sprite.rotationRadians);
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
            sprite.color,
            sprite.artisticIntensity,
            sprite.dissolveThreshold,
            sprite.contributesBloom);
    }
    return vertices;
}

[[nodiscard]] std::vector<SpriteVertex> makeTrailVertices(
    const std::span<const bafx::fx::TrailPoint> trail,
    const float trailWidthPixels)
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
        vertices.push_back(makeVertex(
            vertex.x,
            vertex.y,
            1.0F - vertex.progress,
            vertex.transverse,
            bafx::fx::ColorF{
                trailColor.r,
                trailColor.g,
                trailColor.b,
                1.0F},
            trailArtisticIntensity,
            0.0F,
            true));
    }
    return vertices;
}

}

struct FxGpuRenderer::Implementation
{
    Implementation(
        ID3D11Device* sourceDevice,
        ID3D11DeviceContext* sourceContext,
        const WindowSize initialSize,
        const FxBloomSettings initialBloomSettings)
        : device(sourceDevice)
        , context(sourceContext)
        , size(initialSize)
        , bloomSettings(initialBloomSettings)
    {
        if (!hasValidBloomSettings(bloomSettings))
        {
            throw std::invalid_argument("FX Bloom settings are outside the supported range");
        }
        createPipeline();
        createTextures();
        createTargets();
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
        throwIfFailed(
            device->CreateBlendState(&crossDescription, &crossBlendState),
            "ID3D11Device::CreateBlendState(Cross)");

        D3D11_BLEND_DESC emissionDescription{};
        emissionDescription.IndependentBlendEnable = TRUE;
        emissionDescription.RenderTarget[0] = additiveBlendTarget();
        emissionDescription.RenderTarget[1] = additiveBlendTarget();
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
            unityBloomShaderSource,
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
        createBloomPixelShader("DownsamplePixel", downsamplePixelShader);
        createBloomPixelShader("UpsamplePixel", upsamplePixelShader);
        createBloomPixelShader("CompositePixel", compositePixelShader);
        createBloomPixelShader("DesktopCompositePixel", desktopCompositePixelShader);

        D3D11_BUFFER_DESC constantDescription{};
        constantDescription.ByteWidth = sizeof(BloomConstants);
        constantDescription.Usage = D3D11_USAGE_DYNAMIC;
        constantDescription.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        constantDescription.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        throwIfFailed(
            device->CreateBuffer(&constantDescription, nullptr, &bloomConstantsBuffer),
            "ID3D11Device::CreateBuffer(Bloom constants)");

        D3D11_RASTERIZER_DESC rasterizerDescription{};
        rasterizerDescription.FillMode = D3D11_FILL_SOLID;
        rasterizerDescription.CullMode = D3D11_CULL_NONE;
        rasterizerDescription.DepthClipEnable = TRUE;
        throwIfFailed(
            device->CreateRasterizerState(
                &rasterizerDescription,
                &fullscreenRasterizerState),
            "ID3D11Device::CreateRasterizerState(Bloom)");

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
            unityBloomShaderSource,
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
        circleTexture = loadSrgbTexture(
            device.Get(),
            embeddedUnityTexture(EmbeddedUnityTextureId::Circle01).pngBytes);
        ringTexture = loadSrgbTexture(
            device.Get(),
            embeddedUnityTexture(EmbeddedUnityTextureId::GradRing3).pngBytes);
        triangleTexture = loadSrgbTexture(
            device.Get(),
            embeddedUnityTexture(EmbeddedUnityTextureId::Triangle02_1).pngBytes);
        trailTexture = loadSrgbTexture(
            device.Get(),
            embeddedUnityTexture(EmbeddedUnityTextureId::Trail03).pngBytes);
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
                    unityBloomIntensity});
        if (planResult.status != bafx::core::UnityBloomStatus::Ok)
        {
            throw std::runtime_error("Unity Bloom planner rejected the swap-chain extent");
        }
        bloomPlan = planResult.plan;
        // The config is intentionally a multiplier around the captured Unity
        // exposure, so one leaves the golden/default output unchanged.
        bloomPlan.exposureGain *= bloomSettings.intensityMultiplier;
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
    }

    void createTargets()
    {
        directTarget = createColorTarget(device.Get(), size);
        bloomSeedTarget = createColorTarget(device.Get(), size);
        updateBloomPlan();
        createBloomTargets();
    }

    void unbindFrameResources()
    {
        context->OMSetRenderTargets(0, nullptr, nullptr);
        constexpr std::array<ID3D11ShaderResourceView*, 2> noResources{nullptr, nullptr};
        context->PSSetShaderResources(
            0,
            static_cast<UINT>(noResources.size()),
            noResources.data());
    }

    void releaseSizeDependentTargets()
    {
        unbindFrameResources();
        directTarget = {};
        bloomSeedTarget = {};
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
            return;
        }

        // Diffusion changes mip extents, so release just the pyramid before
        // replacing it. Direct/coverage targets remain valid for this size.
        unbindFrameResources();
        bloomDownTargets.clear();
        bloomUpTargets.clear();
        updateBloomPlan();
        createBloomTargets();
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

    [[nodiscard]] static BloomConstants makeBloomConstants(
        const bafx::core::BloomExtent sourceExtent,
        const float sampleScale,
        const float exposureGain,
        const float backgroundWeight = 0.0F) noexcept
    {
        BloomConstants constants{};
        constants.sourceTexelSize[0] = 1.0F / static_cast<float>(sourceExtent.width);
        constants.sourceTexelSize[1] = 1.0F / static_cast<float>(sourceExtent.height);
        constants.sampleScale = sampleScale;
        constants.exposureGain = exposureGain;
        constants.padding = backgroundWeight;
        return constants;
    }

    void drawFullscreen(
        ID3D11RenderTargetView* target,
        const bafx::core::BloomExtent targetExtent,
        ID3D11PixelShader* pixelShader,
        ID3D11ShaderResourceView* source0,
        ID3D11ShaderResourceView* source1,
        const BloomConstants& constants)
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
        context->RSSetState(fullscreenRasterizerState.Get());
        context->OMSetDepthStencilState(depthState.Get(), 0);
        context->OMSetRenderTargets(1, &target, nullptr);
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
        std::array<ID3D11ShaderResourceView*, 2> sources{source0, source1};
        context->PSSetShaderResources(
            0,
            static_cast<UINT>(sources.size()),
            sources.data());
        context->Draw(3, 0);

        constexpr std::array<ID3D11ShaderResourceView*, 2> noResources{
            nullptr,
            nullptr};
        context->PSSetShaderResources(
            0,
            static_cast<UINT>(noResources.size()),
            noResources.data());
    }

    void renderBloom(
        ID3D11RenderTargetView* destination,
        ID3D11PixelShader* finalCompositeShader,
        const std::optional<BackgroundRenderInput> background)
    {
        const bafx::core::BloomExtent sourceExtent{
            static_cast<std::int32_t>(size.width),
            static_cast<std::int32_t>(size.height)};
        const bafx::core::BloomExtent firstExtent = bloomPlan.mipChain[0];
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
                background.has_value() ? background->freshnessWeight : 0.0F));

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
                    0.0F));
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
                    0.0F));
            accumulated = bloomUpTargets[fineIndex].shaderResource.Get();
        }

        drawFullscreen(
            destination,
            sourceExtent,
            finalCompositeShader,
            directTarget.shaderResource.Get(),
            accumulated,
            makeBloomConstants(
                firstExtent,
                bloomPlan.sampleScale,
                bloomPlan.exposureGain));
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

    void drawSprite(const bafx::fx::Sprite& sprite)
    {
        switch (sprite.kind)
        {
        case bafx::fx::SpriteKind::CenterDisk:
        {
            const std::array<SpriteVertex, 6> vertices = makeSpriteVertices(sprite);
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
            const auto vertices = makeRingVertices(sprite);
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
            const std::array<SpriteVertex, 6> vertices = makeSpriteVertices(sprite);
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

    void render(
        const bafx::fx::FrameSnapshot& snapshot,
        ID3D11RenderTargetView* destination,
        ID3D11PixelShader* finalCompositeShader,
        std::optional<BackgroundRenderInput> background = std::nullopt)
    {
        if (background.has_value()
            && (background->shaderResource == nullptr
                || !std::isfinite(background->freshnessWeight)
                || background->freshnessWeight < 0.0F))
        {
            background.reset();
        }
        else if (background.has_value())
        {
            background->freshnessWeight = std::clamp(
                background->freshnessWeight,
                0.0F,
                1.0F);
        }

        constexpr std::array<float, 4> transparent{0.0F, 0.0F, 0.0F, 0.0F};
        if (!hasVisualContent(snapshot))
        {
            context->ClearRenderTargetView(destination, transparent.data());
            return;
        }
        const ComPtr<ID3D11Texture2D> backgroundTexture = background.has_value()
            ? textureFromShaderResource(background->shaderResource)
            : ComPtr<ID3D11Texture2D>{};
        if (background.has_value()
            && !isCompatibleBackgroundTexture(backgroundTexture.Get(), size))
        {
            background.reset();
        }

        // WGC is a Bloom input only. Keeping both material targets transparent
        // preserves the same direct/coverage contract in every capture state.
        context->ClearRenderTargetView(
            directTarget.renderTarget.Get(),
            transparent.data());
        context->ClearRenderTargetView(
            bloomSeedTarget.renderTarget.Get(),
            transparent.data());
        std::array<ID3D11RenderTargetView*, 2> targets{
            directTarget.renderTarget.Get(),
            bloomSeedTarget.renderTarget.Get()};
        context->OMSetRenderTargets(
            static_cast<UINT>(targets.size()),
            targets.data(),
            nullptr);
        configureFramePipeline();

        std::size_t index = 0U;
        while (index < snapshot.sprites.size()
            && snapshot.sprites[index].renderQueue <= trailRenderQueue)
        {
            drawSprite(snapshot.sprites[index]);
            ++index;
        }
        const auto drawTrail = [this](
                                   const std::span<const bafx::fx::TrailPoint> points,
                                   const float widthPixels)
        {
            const std::vector<SpriteVertex> trailVertices = makeTrailVertices(
                points,
                widthPixels);
            drawVertices(
                trailVertices,
                trailTexture.Get(),
                repeatSampler.Get(),
                additivePixelShader.Get(),
                emissionBlendState.Get());
        };
        if (snapshot.trailStrokes.empty())
        {
            drawTrail(snapshot.trail, snapshot.trailWidthPixels);
        }
        else
        {
            for (const bafx::fx::TrailStroke& stroke : snapshot.trailStrokes)
            {
                drawTrail(stroke.points, stroke.widthPixels);
            }
        }
        while (index < snapshot.sprites.size())
        {
            drawSprite(snapshot.sprites[index]);
            ++index;
        }

        // Unbind the material MRTs before sampling them through the Bloom chain.
        context->OMSetRenderTargets(0, nullptr, nullptr);
        renderBloom(destination, finalCompositeShader, background);
        context->OMSetRenderTargets(0, nullptr, nullptr);
    }

    [[nodiscard]] FxGpuFrameCapture renderAndCapture(
        const bafx::fx::FrameSnapshot& snapshot,
        ID3D11RenderTargetView* destination)
    {
        render(snapshot, destination, compositePixelShader.Get());

        FxGpuFrameCapture capture{};
        const ComPtr<ID3D11Texture2D> destinationTexture =
            textureFromRenderTarget(destination);
        capture.finalOverlay = readbackRgba16FloatTexture(
            context.Get(),
            destinationTexture.Get());
        if (!hasVisualContent(snapshot))
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
        capture.intermediateLayersValid = true;
        return capture;
    }

    ComPtr<ID3D11Device> device{};
    ComPtr<ID3D11DeviceContext> context{};
    WindowSize size{};
    FxBloomSettings bloomSettings{};
    ColorTarget directTarget{};
    ColorTarget bloomSeedTarget{};
    bafx::core::UnityBloomPlan bloomPlan{};
    std::vector<ColorTarget> bloomDownTargets{};
    std::vector<ColorTarget> bloomUpTargets{};
    ComPtr<ID3D11VertexShader> vertexShader{};
    ComPtr<ID3D11VertexShader> fullscreenVertexShader{};
    ComPtr<ID3D11PixelShader> crossPixelShader{};
    ComPtr<ID3D11PixelShader> dissolvePixelShader{};
    ComPtr<ID3D11PixelShader> additivePixelShader{};
    ComPtr<ID3D11PixelShader> prefilterPixelShader{};
    ComPtr<ID3D11PixelShader> differentialPrefilterPixelShader{};
    ComPtr<ID3D11PixelShader> downsamplePixelShader{};
    ComPtr<ID3D11PixelShader> upsamplePixelShader{};
    ComPtr<ID3D11PixelShader> compositePixelShader{};
    ComPtr<ID3D11PixelShader> desktopCompositePixelShader{};
    ComPtr<ID3D11InputLayout> inputLayout{};
    ComPtr<ID3D11Buffer> vertexBuffer{};
    ComPtr<ID3D11Buffer> viewportBuffer{};
    ComPtr<ID3D11Buffer> bloomConstantsBuffer{};
    ComPtr<ID3D11SamplerState> clampSampler{};
    ComPtr<ID3D11SamplerState> repeatSampler{};
    ComPtr<ID3D11RasterizerState> rasterizerState{};
    ComPtr<ID3D11RasterizerState> fullscreenRasterizerState{};
    ComPtr<ID3D11DepthStencilState> depthState{};
    ComPtr<ID3D11BlendState> crossBlendState{};
    ComPtr<ID3D11BlendState> emissionBlendState{};
    ComPtr<ID3D11BlendState> overwriteBlendState{};
    ComPtr<ID3D11ShaderResourceView> circleTexture{};
    ComPtr<ID3D11ShaderResourceView> ringTexture{};
    ComPtr<ID3D11ShaderResourceView> triangleTexture{};
    ComPtr<ID3D11ShaderResourceView> trailTexture{};
    std::size_t vertexCapacity{0U};
};

FxGpuRenderer::FxGpuRenderer(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    const WindowSize size,
    const FxBloomSettings bloomSettings)
    : implementation_(std::make_unique<Implementation>(
        device,
        context,
        size,
        bloomSettings))
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

void FxGpuRenderer::render(
    const bafx::fx::FrameSnapshot& snapshot,
    ID3D11RenderTargetView* destination,
    const std::optional<BackgroundRenderInput> background)
{
    implementation_->render(
        snapshot,
        destination,
        implementation_->desktopCompositePixelShader.Get(),
        background);
}

FxGpuFrameCapture FxGpuRenderer::renderAndCapture(
    const bafx::fx::FrameSnapshot& snapshot,
    ID3D11RenderTargetView* destination)
{
    return implementation_->renderAndCapture(snapshot, destination);
}

}
