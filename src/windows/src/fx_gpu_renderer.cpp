#include "bafx/windows/fx_gpu_renderer.hpp"

#include "bafx/reference/unity_runtime_resources.hpp"
#include "bafx/windows/error.hpp"
#include "wic_texture_loader.hpp"

#include <d3dcompiler.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <numbers>
#include <span>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace bafx::windows
{
namespace
{

using Microsoft::WRL::ComPtr;

constexpr float trailArtisticIntensity = 23.968628F;
constexpr std::int32_t trailRenderQueue = 4499;
constexpr std::size_t initialVertexCapacity = 256U;

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

struct ColorTarget
{
    ComPtr<ID3D11Texture2D> texture{};
    ComPtr<ID3D11RenderTargetView> renderTarget{};
    ComPtr<ID3D11ShaderResourceView> shaderResource{};
};

enum class ProceduralTextureKind : std::uint8_t
{
    Circle,
    Ring,
    TriangleAtlas,
    Trail
};

[[nodiscard]] std::filesystem::path executableDirectory()
{
    std::array<wchar_t, 32768> buffer{};
    const DWORD length = GetModuleFileNameW(
        nullptr,
        buffer.data(),
        static_cast<DWORD>(buffer.size()));
    if (length == 0U || length >= buffer.size())
    {
        throwLastError("GetModuleFileNameW");
    }
    return std::filesystem::path(buffer.data(), buffer.data() + length).parent_path();
}

[[nodiscard]] ComPtr<ID3DBlob> compileShader(
    const std::filesystem::path& path,
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
    const HRESULT result = D3DCompileFromFile(
        path.c_str(),
        nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        entryPoint,
        profile,
        flags,
        0,
        &byteCode,
        &errors);
    if (FAILED(result))
    {
        std::string message = "D3DCompileFromFile failed";
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

[[nodiscard]] D3D11_RENDER_TARGET_BLEND_DESC additiveBlendTarget(
    const bool preserveAlpha) noexcept
{
    D3D11_RENDER_TARGET_BLEND_DESC target{};
    target.BlendEnable = TRUE;
    target.SrcBlend = D3D11_BLEND_ONE;
    target.DestBlend = D3D11_BLEND_ONE;
    target.BlendOp = D3D11_BLEND_OP_ADD;
    target.SrcBlendAlpha = preserveAlpha ? D3D11_BLEND_ZERO : D3D11_BLEND_ONE;
    target.DestBlendAlpha = D3D11_BLEND_ONE;
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

[[nodiscard]] std::uint8_t unitToByte(const float value) noexcept
{
    return static_cast<std::uint8_t>(
        std::clamp(value, 0.0F, 1.0F) * 255.0F + 0.5F);
}

[[nodiscard]] std::uint32_t rgba(
    const float red,
    const float green,
    const float blue,
    const float alpha) noexcept
{
    return static_cast<std::uint32_t>(unitToByte(red))
        | (static_cast<std::uint32_t>(unitToByte(green)) << 8U)
        | (static_cast<std::uint32_t>(unitToByte(blue)) << 16U)
        | (static_cast<std::uint32_t>(unitToByte(alpha)) << 24U);
}

[[nodiscard]] float smoothCoverage(
    const float distance,
    const float edge,
    const float softness) noexcept
{
    return std::clamp((edge - distance) / softness + 0.5F, 0.0F, 1.0F);
}

[[nodiscard]] std::vector<std::uint32_t> makeCirclePixels(
    const std::uint32_t width,
    const std::uint32_t height)
{
    std::vector<std::uint32_t> pixels(
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    for (std::uint32_t y = 0; y < height; ++y)
    {
        for (std::uint32_t x = 0; x < width; ++x)
        {
            const float px = (static_cast<float>(x) + 0.5F) / static_cast<float>(width) - 0.5F;
            const float py = (static_cast<float>(y) + 0.5F) / static_cast<float>(height) - 0.5F;
            const float shape = smoothCoverage(std::sqrt(px * px + py * py), 0.42F, 0.025F);
            pixels[static_cast<std::size_t>(y) * width + x] = rgba(shape, shape, shape, 1.0F);
        }
    }
    return pixels;
}

[[nodiscard]] std::vector<std::uint32_t> makeRingPixels(
    const std::uint32_t width,
    const std::uint32_t height)
{
    std::vector<std::uint32_t> pixels(
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    for (std::uint32_t y = 0; y < height; ++y)
    {
        for (std::uint32_t x = 0; x < width; ++x)
        {
            const float px = (static_cast<float>(x) + 0.5F) / static_cast<float>(width) - 0.5F;
            const float py = (static_cast<float>(y) + 0.5F) / static_cast<float>(height) - 0.5F;
            const float radius = std::sqrt(px * px + py * py);
            const float shape = smoothCoverage(std::abs(radius - 0.34F), 0.035F, 0.018F);
            pixels[static_cast<std::size_t>(y) * width + x] = rgba(1.0F, 1.0F, 1.0F, shape);
        }
    }
    return pixels;
}

[[nodiscard]] float triangleCoverage(const float x, const float y) noexcept
{
    const float leftEdge = y + 0.82F * x + 0.35F;
    const float rightEdge = y - 0.82F * x + 0.35F;
    const float bottomEdge = 0.42F - y;
    const float edge = std::min({leftEdge, rightEdge, bottomEdge});
    return std::clamp(edge / 0.025F + 0.5F, 0.0F, 1.0F);
}

[[nodiscard]] std::vector<std::uint32_t> makeTrianglePixels(
    const std::uint32_t width,
    const std::uint32_t height)
{
    std::vector<std::uint32_t> pixels(
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    const std::uint32_t frameWidth = width / 2U;
    for (std::uint32_t y = 0; y < height; ++y)
    {
        for (std::uint32_t x = 0; x < width; ++x)
        {
            const std::uint32_t localX = x % frameWidth;
            float px = (static_cast<float>(localX) + 0.5F)
                / static_cast<float>(frameWidth) - 0.5F;
            if (x >= frameWidth)
            {
                px = -px;
            }
            const float py = (static_cast<float>(y) + 0.5F)
                / static_cast<float>(height) - 0.5F;
            const float shape = triangleCoverage(px, py);
            pixels[static_cast<std::size_t>(y) * width + x] = rgba(1.0F, 1.0F, 1.0F, shape);
        }
    }
    return pixels;
}

[[nodiscard]] std::vector<std::uint32_t> makeTrailPixels(
    const std::uint32_t width,
    const std::uint32_t height)
{
    std::vector<std::uint32_t> pixels(
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    for (std::uint32_t y = 0; y < height; ++y)
    {
        const float v = (static_cast<float>(y) + 0.5F) / static_cast<float>(height);
        const float shape = std::pow(std::sin(v * std::numbers::pi_v<float>), 1.5F);
        for (std::uint32_t x = 0; x < width; ++x)
        {
            pixels[static_cast<std::size_t>(y) * width + x] = rgba(shape, shape, shape, 1.0F);
        }
    }
    return pixels;
}

[[nodiscard]] ComPtr<ID3D11ShaderResourceView> createProceduralTexture(
    ID3D11Device* device,
    const ProceduralTextureKind kind)
{
    const bool atlas = kind == ProceduralTextureKind::TriangleAtlas;
    const std::uint32_t width = atlas ? 256U : 256U;
    const std::uint32_t height = atlas ? 128U : 256U;

    std::vector<std::uint32_t> pixels;
    switch (kind)
    {
    case ProceduralTextureKind::Circle:
        pixels = makeCirclePixels(width, height);
        break;

    case ProceduralTextureKind::Ring:
        pixels = makeRingPixels(width, height);
        break;

    case ProceduralTextureKind::TriangleAtlas:
        pixels = makeTrianglePixels(width, height);
        break;

    case ProceduralTextureKind::Trail:
        pixels = makeTrailPixels(width, height);
        break;
    }

    D3D11_TEXTURE2D_DESC description{};
    description.Width = width;
    description.Height = height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    description.SampleDesc = DXGI_SAMPLE_DESC{1, 0};
    description.Usage = D3D11_USAGE_IMMUTABLE;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA data{};
    data.pSysMem = pixels.data();
    data.SysMemPitch = width * sizeof(std::uint32_t);

    ComPtr<ID3D11Texture2D> texture;
    throwIfFailed(
        device->CreateTexture2D(&description, &data, &texture),
        "ID3D11Device::CreateTexture2D(procedural FX texture)");
    ComPtr<ID3D11ShaderResourceView> resource;
    throwIfFailed(
        device->CreateShaderResourceView(texture.Get(), nullptr, &resource),
        "ID3D11Device::CreateShaderResourceView(procedural FX texture)");
    return resource;
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

[[nodiscard]] bafx::fx::ColorF trailColor(const float normalizedAge) noexcept
{
    const float age = std::clamp(normalizedAge, 0.0F, 1.0F);
    if (age <= 0.45F)
    {
        const float amount = age / 0.45F;
        return bafx::fx::ColorF{
            0.0F,
            0.391F + (0.08F - 0.391F) * amount,
            1.0F + (0.35F - 1.0F) * amount,
            1.0F};
    }
    const float amount = (age - 0.45F) / 0.55F;
    return bafx::fx::ColorF{
        0.0F,
        0.08F * (1.0F - amount),
        0.35F * (1.0F - amount),
        1.0F};
}

[[nodiscard]] std::vector<SpriteVertex> makeTrailVertices(
    const bafx::fx::FrameSnapshot& snapshot)
{
    std::vector<SpriteVertex> vertices;
    if (snapshot.trail.size() < 2U || snapshot.trailWidthPixels <= 0.0F)
    {
        return vertices;
    }

    struct TrailPair
    {
        SpriteVertex top{};
        SpriteVertex bottom{};
    };
    std::vector<TrailPair> pairs;
    pairs.reserve(snapshot.trail.size());
    const float halfWidth = snapshot.trailWidthPixels * 0.5F;
    for (std::size_t index = 0; index < snapshot.trail.size(); ++index)
    {
        const std::size_t previous = index == 0U ? 0U : index - 1U;
        const std::size_t next = std::min(index + 1U, snapshot.trail.size() - 1U);
        const auto& point = snapshot.trail[index];
        const float tangentX = snapshot.trail[next].positionPixels.x
            - snapshot.trail[previous].positionPixels.x;
        const float tangentY = snapshot.trail[next].positionPixels.y
            - snapshot.trail[previous].positionPixels.y;
        const float tangentLength = std::sqrt(tangentX * tangentX + tangentY * tangentY);
        if (tangentLength <= std::numeric_limits<float>::epsilon())
        {
            continue;
        }
        const float normalX = -tangentY / tangentLength;
        const float normalY = tangentX / tangentLength;
        const float u = static_cast<float>(index)
            / static_cast<float>(snapshot.trail.size() - 1U);
        const bafx::fx::ColorF color = trailColor(point.normalizedAge);
        pairs.push_back(TrailPair{
            makeVertex(
                point.positionPixels.x + normalX * halfWidth,
                point.positionPixels.y + normalY * halfWidth,
                u,
                0.0F,
                color,
                trailArtisticIntensity,
                0.0F,
                true),
            makeVertex(
                point.positionPixels.x - normalX * halfWidth,
                point.positionPixels.y - normalY * halfWidth,
                u,
                1.0F,
                color,
                trailArtisticIntensity,
                0.0F,
                true)});
    }

    if (pairs.size() < 2U)
    {
        return vertices;
    }
    vertices.reserve((pairs.size() - 1U) * 6U);
    for (std::size_t index = 1; index < pairs.size(); ++index)
    {
        const TrailPair& previous = pairs[index - 1U];
        const TrailPair& current = pairs[index];
        vertices.push_back(previous.top);
        vertices.push_back(current.top);
        vertices.push_back(current.bottom);
        vertices.push_back(previous.top);
        vertices.push_back(current.bottom);
        vertices.push_back(previous.bottom);
    }
    return vertices;
}

}

struct FxGpuRenderer::Implementation
{
    Implementation(
        ID3D11Device* sourceDevice,
        ID3D11DeviceContext* sourceContext,
        const WindowSize initialSize)
        : device(sourceDevice)
        , context(sourceContext)
        , size(initialSize)
    {
        createPipeline();
        createTextures();
        createTargets();
    }

    void createPipeline()
    {
        const std::filesystem::path shaderPath = executableDirectory()
            / L"shaders"
            / L"fx_materials.hlsl";
        const ComPtr<ID3DBlob> vertexByteCode = compileShader(
            shaderPath,
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

        createPixelShader(shaderPath, "CrossPixel", crossPixelShader);
        createPixelShader(shaderPath, "DissolvePixel", dissolvePixelShader);
        createPixelShader(shaderPath, "AdditivePixel", additivePixelShader);

        D3D11_BUFFER_DESC constantDescription{};
        constantDescription.ByteWidth = sizeof(ViewportConstants);
        constantDescription.Usage = D3D11_USAGE_DYNAMIC;
        constantDescription.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        constantDescription.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        throwIfFailed(
            device->CreateBuffer(&constantDescription, nullptr, &viewportBuffer),
            "ID3D11Device::CreateBuffer(FX viewport)");

        createVertexBuffer(initialVertexCapacity);

        D3D11_SAMPLER_DESC samplerDescription{};
        samplerDescription.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        samplerDescription.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDescription.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDescription.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDescription.MinLOD = 0.0F;
        samplerDescription.MaxLOD = D3D11_FLOAT32_MAX;
        throwIfFailed(
            device->CreateSamplerState(&samplerDescription, &sampler),
            "ID3D11Device::CreateSamplerState(FX)");

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
        crossDescription.RenderTarget[1] = additiveBlendTarget(true);
        throwIfFailed(
            device->CreateBlendState(&crossDescription, &crossBlendState),
            "ID3D11Device::CreateBlendState(Cross)");

        D3D11_BLEND_DESC emissionDescription{};
        emissionDescription.IndependentBlendEnable = TRUE;
        emissionDescription.RenderTarget[0] = additiveBlendTarget(true);
        emissionDescription.RenderTarget[1] = additiveBlendTarget(true);
        throwIfFailed(
            device->CreateBlendState(&emissionDescription, &emissionBlendState),
            "ID3D11Device::CreateBlendState(emission)");
    }

    void createPixelShader(
        const std::filesystem::path& path,
        const char* entryPoint,
        ComPtr<ID3D11PixelShader>& output)
    {
        const ComPtr<ID3DBlob> byteCode = compileShader(path, entryPoint, "ps_5_0");
        throwIfFailed(
            device->CreatePixelShader(
                byteCode->GetBufferPointer(),
                byteCode->GetBufferSize(),
                nullptr,
                &output),
            "ID3D11Device::CreatePixelShader(FX)");
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
        const bafx::reference::UnityRuntimeLocationResult location =
            bafx::reference::locateUnityRuntimeResources();
        if (const auto* resources =
                std::get_if<bafx::reference::UnityRuntimeResources>(&location))
        {
            circleTexture = loadSrgbTexture(device.Get(), resources->circle01);
            ringTexture = loadSrgbTexture(device.Get(), resources->gradRing3);
            triangleTexture = loadSrgbTexture(device.Get(), resources->triangle02_1);
            trailTexture = loadSrgbTexture(device.Get(), resources->trail03);
            return;
        }

        const auto& error = std::get<bafx::reference::UnityRuntimeLocationError>(location);
        const std::string message = error.message()
            + " Falling back to generated development textures.\n";
        OutputDebugStringA(message.c_str());
        circleTexture = createProceduralTexture(device.Get(), ProceduralTextureKind::Circle);
        ringTexture = createProceduralTexture(device.Get(), ProceduralTextureKind::Ring);
        triangleTexture = createProceduralTexture(
            device.Get(),
            ProceduralTextureKind::TriangleAtlas);
        trailTexture = createProceduralTexture(device.Get(), ProceduralTextureKind::Trail);
    }

    void createTargets()
    {
        directTarget = createColorTarget(device.Get(), size);
        bloomSeedTarget = createColorTarget(device.Get(), size);
    }

    void resize(const WindowSize nextSize)
    {
        if (nextSize.width == 0U || nextSize.height == 0U
            || (nextSize.width == size.width && nextSize.height == size.height))
        {
            return;
        }
        context->OMSetRenderTargets(0, nullptr, nullptr);
        directTarget = {};
        bloomSeedTarget = {};
        size = nextSize;
        createTargets();
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
        ID3D11SamplerState* materialSampler = sampler.Get();
        context->PSSetSamplers(0, 1, &materialSampler);
    }

    void drawVertices(
        const std::span<const SpriteVertex> vertices,
        ID3D11ShaderResourceView* texture,
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
        context->Draw(static_cast<UINT>(vertices.size()), 0);
    }

    void drawSprite(const bafx::fx::Sprite& sprite)
    {
        const std::array<SpriteVertex, 6> vertices = makeSpriteVertices(sprite);
        switch (sprite.kind)
        {
        case bafx::fx::SpriteKind::CenterDisk:
            drawVertices(
                vertices,
                circleTexture.Get(),
                crossPixelShader.Get(),
                crossBlendState.Get());
            break;

        case bafx::fx::SpriteKind::DissolveRing:
            drawVertices(
                vertices,
                ringTexture.Get(),
                dissolvePixelShader.Get(),
                emissionBlendState.Get());
            break;

        case bafx::fx::SpriteKind::Triangle:
            drawVertices(
                vertices,
                triangleTexture.Get(),
                additivePixelShader.Get(),
                emissionBlendState.Get());
            break;
        }
    }

    void render(const bafx::fx::FrameSnapshot& snapshot, ID3D11Texture2D* destination)
    {
        constexpr std::array<float, 4> transparent{0.0F, 0.0F, 0.0F, 0.0F};
        context->ClearRenderTargetView(directTarget.renderTarget.Get(), transparent.data());
        context->ClearRenderTargetView(bloomSeedTarget.renderTarget.Get(), transparent.data());
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
        const std::vector<SpriteVertex> trailVertices = makeTrailVertices(snapshot);
        drawVertices(
            trailVertices,
            trailTexture.Get(),
            additivePixelShader.Get(),
            emissionBlendState.Get());
        while (index < snapshot.sprites.size())
        {
            drawSprite(snapshot.sprites[index]);
            ++index;
        }

        // The game additive target alpha is intentionally discarded; directTarget owns DComp alpha.
        context->OMSetRenderTargets(0, nullptr, nullptr);
        context->CopyResource(destination, directTarget.texture.Get());
    }

    ComPtr<ID3D11Device> device{};
    ComPtr<ID3D11DeviceContext> context{};
    WindowSize size{};
    ColorTarget directTarget{};
    ColorTarget bloomSeedTarget{};
    ComPtr<ID3D11VertexShader> vertexShader{};
    ComPtr<ID3D11PixelShader> crossPixelShader{};
    ComPtr<ID3D11PixelShader> dissolvePixelShader{};
    ComPtr<ID3D11PixelShader> additivePixelShader{};
    ComPtr<ID3D11InputLayout> inputLayout{};
    ComPtr<ID3D11Buffer> vertexBuffer{};
    ComPtr<ID3D11Buffer> viewportBuffer{};
    ComPtr<ID3D11SamplerState> sampler{};
    ComPtr<ID3D11RasterizerState> rasterizerState{};
    ComPtr<ID3D11DepthStencilState> depthState{};
    ComPtr<ID3D11BlendState> crossBlendState{};
    ComPtr<ID3D11BlendState> emissionBlendState{};
    ComPtr<ID3D11ShaderResourceView> circleTexture{};
    ComPtr<ID3D11ShaderResourceView> ringTexture{};
    ComPtr<ID3D11ShaderResourceView> triangleTexture{};
    ComPtr<ID3D11ShaderResourceView> trailTexture{};
    std::size_t vertexCapacity{0U};
};

FxGpuRenderer::FxGpuRenderer(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    const WindowSize size)
    : implementation_(std::make_unique<Implementation>(device, context, size))
{
}

FxGpuRenderer::~FxGpuRenderer() = default;

void FxGpuRenderer::resize(const WindowSize size)
{
    implementation_->resize(size);
}

void FxGpuRenderer::render(
    const bafx::fx::FrameSnapshot& snapshot,
    ID3D11Texture2D* destination)
{
    implementation_->render(snapshot, destination);
}

}
