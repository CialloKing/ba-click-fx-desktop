#include "test_support.hpp"

#include "embedded_fx_shaders.hpp"

#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <array>
#include <stdexcept>
#include <string>
#include <string_view>

using Microsoft::WRL::ComPtr;
using namespace bafx::windows;

namespace
{

struct ShaderEntry
{
    std::string_view source{};
    const char* entryPoint{nullptr};
    const char* profile{nullptr};
};

[[nodiscard]] ComPtr<ID3DBlob> compileShader(const ShaderEntry& entry)
{
    ComPtr<ID3DBlob> byteCode;
    ComPtr<ID3DBlob> errors;
    const HRESULT result = D3DCompile(
        entry.source.data(),
        entry.source.size(),
        "embedded_fx_shaders.hpp",
        nullptr,
        nullptr,
        entry.entryPoint,
        entry.profile,
        D3DCOMPILE_ENABLE_STRICTNESS
            | D3DCOMPILE_WARNINGS_ARE_ERRORS
            | D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0,
        &byteCode,
        &errors);
    if (FAILED(result))
    {
        std::string message = "D3DCompile failed for ";
        message += entry.entryPoint;
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

[[nodiscard]] ComPtr<ID3D11ShaderReflection> reflectShader(ID3DBlob* byteCode)
{
    ComPtr<ID3D11ShaderReflection> reflection;
    const HRESULT result = D3DReflect(
        byteCode->GetBufferPointer(),
        byteCode->GetBufferSize(),
        __uuidof(ID3D11ShaderReflection),
        reinterpret_cast<void**>(reflection.GetAddressOf()));
    if (FAILED(result))
    {
        throw std::runtime_error("D3DReflect failed");
    }
    return reflection;
}

[[nodiscard]] bool hasBinding(
    ID3D11ShaderReflection* reflection,
    const char* name,
    const D3D_SHADER_INPUT_TYPE type,
    const UINT bindPoint)
{
    D3D11_SHADER_INPUT_BIND_DESC description{};
    return SUCCEEDED(reflection->GetResourceBindingDescByName(name, &description))
        && description.Type == type
        && description.BindPoint == bindPoint
        && description.BindCount == 1U;
}

[[nodiscard]] bool hasSignatureParameter(
    ID3D11ShaderReflection* reflection,
    const bool output,
    const std::string_view semantic,
    const UINT semanticIndex,
    const BYTE mask)
{
    D3D11_SHADER_DESC shaderDescription{};
    if (FAILED(reflection->GetDesc(&shaderDescription)))
    {
        return false;
    }

    const UINT count = output
        ? shaderDescription.OutputParameters
        : shaderDescription.InputParameters;
    for (UINT index = 0U; index < count; ++index)
    {
        D3D11_SIGNATURE_PARAMETER_DESC parameter{};
        const HRESULT result = output
            ? reflection->GetOutputParameterDesc(index, &parameter)
            : reflection->GetInputParameterDesc(index, &parameter);
        if (SUCCEEDED(result)
            && parameter.SemanticName != nullptr
            && std::string_view(parameter.SemanticName) == semantic
            && parameter.SemanticIndex == semanticIndex
            && parameter.Mask == mask)
        {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool hasConstantVariable(
    ID3D11ShaderReflection* reflection,
    const char* bufferName,
    const char* variableName,
    const UINT offset,
    const UINT size)
{
    ID3D11ShaderReflectionConstantBuffer* buffer =
        reflection->GetConstantBufferByName(bufferName);
    if (buffer == nullptr)
    {
        return false;
    }
    D3D11_SHADER_BUFFER_DESC bufferDescription{};
    if (FAILED(buffer->GetDesc(&bufferDescription)))
    {
        return false;
    }
    ID3D11ShaderReflectionVariable* variable = buffer->GetVariableByName(variableName);
    if (variable == nullptr)
    {
        return false;
    }
    D3D11_SHADER_VARIABLE_DESC variableDescription{};
    return SUCCEEDED(variable->GetDesc(&variableDescription))
        && variableDescription.StartOffset == offset
        && variableDescription.Size == size;
}

[[nodiscard]] ComPtr<ID3D11ShaderReflection> compileAndReflect(
    const ShaderEntry& entry)
{
    const ComPtr<ID3DBlob> byteCode = compileShader(entry);
    return reflectShader(byteCode.Get());
}

}

BAFX_TEST(all_embedded_fx_shader_entries_compile_with_warnings_as_errors)
{
    constexpr std::array entries{
        ShaderEntry{fxMaterialsShaderSource, "SpriteVertex", "vs_5_0"},
        ShaderEntry{fxMaterialsShaderSource, "CrossPixel", "ps_5_0"},
        ShaderEntry{fxMaterialsShaderSource, "DissolvePixel", "ps_5_0"},
        ShaderEntry{fxMaterialsShaderSource, "AdditivePixel", "ps_5_0"},
        ShaderEntry{unityBloomShaderSource, "FullscreenVertex", "vs_5_0"},
        ShaderEntry{unityBloomShaderSource, "PrefilterPixel", "ps_5_0"},
        ShaderEntry{unityBloomShaderSource, "DownsamplePixel", "ps_5_0"},
        ShaderEntry{unityBloomShaderSource, "UpsamplePixel", "ps_5_0"},
        ShaderEntry{unityBloomShaderSource, "CompositePixel", "ps_5_0"}};

    for (const ShaderEntry& entry : entries)
    {
        BAFX_CHECK(compileShader(entry) != nullptr);
    }
}

BAFX_TEST(sprite_shader_reflection_locks_vertex_and_mrt_contracts)
{
    const auto vertex = compileAndReflect(
        ShaderEntry{fxMaterialsShaderSource, "SpriteVertex", "vs_5_0"});
    BAFX_CHECK(hasBinding(vertex.Get(), "ViewportConstants", D3D_SIT_CBUFFER, 0U));
    BAFX_CHECK(hasSignatureParameter(vertex.Get(), false, "POSITION", 0U, 0x3U));
    BAFX_CHECK(hasSignatureParameter(vertex.Get(), false, "TEXCOORD", 0U, 0x3U));
    BAFX_CHECK(hasSignatureParameter(vertex.Get(), false, "COLOR", 0U, 0xFU));
    BAFX_CHECK(hasSignatureParameter(vertex.Get(), false, "TEXCOORD", 1U, 0x1U));
    BAFX_CHECK(hasSignatureParameter(vertex.Get(), false, "TEXCOORD", 2U, 0x1U));
    BAFX_CHECK(hasSignatureParameter(vertex.Get(), false, "TEXCOORD", 3U, 0x1U));
    BAFX_CHECK(hasConstantVariable(
        vertex.Get(), "ViewportConstants", "ViewportSize", 0U, 8U));
    BAFX_CHECK(hasConstantVariable(
        vertex.Get(), "ViewportConstants", "ViewportPadding", 8U, 8U));

    constexpr std::array pixelEntries{
        "CrossPixel",
        "DissolvePixel",
        "AdditivePixel"};
    for (const char* entryPoint : pixelEntries)
    {
        const auto pixel = compileAndReflect(
            ShaderEntry{fxMaterialsShaderSource, entryPoint, "ps_5_0"});
        BAFX_CHECK(hasBinding(pixel.Get(), "MaterialTexture", D3D_SIT_TEXTURE, 0U));
        BAFX_CHECK(hasBinding(pixel.Get(), "MaterialSampler", D3D_SIT_SAMPLER, 0U));
        BAFX_CHECK(hasSignatureParameter(pixel.Get(), true, "SV_Target", 0U, 0xFU));
        BAFX_CHECK(hasSignatureParameter(pixel.Get(), true, "SV_Target", 1U, 0xFU));
    }
}

BAFX_TEST(bloom_shader_reflection_locks_resources_and_constant_layout)
{
    constexpr std::array entries{
        "PrefilterPixel",
        "DownsamplePixel",
        "UpsamplePixel",
        "CompositePixel"};
    for (const char* entryPoint : entries)
    {
        const auto reflection = compileAndReflect(
            ShaderEntry{unityBloomShaderSource, entryPoint, "ps_5_0"});
        BAFX_CHECK(hasBinding(reflection.Get(), "BloomConstants", D3D_SIT_CBUFFER, 0U));
        BAFX_CHECK(hasBinding(reflection.Get(), "Source0", D3D_SIT_TEXTURE, 0U));
        BAFX_CHECK(hasBinding(
            reflection.Get(), "LinearClampSampler", D3D_SIT_SAMPLER, 0U));
        BAFX_CHECK(hasSignatureParameter(
            reflection.Get(), true, "SV_Target", 0U, 0xFU));
        BAFX_CHECK(hasConstantVariable(
            reflection.Get(), "BloomConstants", "SourceTexelSize", 0U, 8U));
        BAFX_CHECK(hasConstantVariable(
            reflection.Get(), "BloomConstants", "SampleScale", 8U, 4U));
        BAFX_CHECK(hasConstantVariable(
            reflection.Get(), "BloomConstants", "ExposureGain", 12U, 4U));
        BAFX_CHECK(hasConstantVariable(
            reflection.Get(), "BloomConstants", "Threshold", 16U, 4U));
        BAFX_CHECK(hasConstantVariable(
            reflection.Get(), "BloomConstants", "Knee", 20U, 4U));
        BAFX_CHECK(hasConstantVariable(
            reflection.Get(), "BloomConstants", "ClampValue", 24U, 4U));
        BAFX_CHECK(hasConstantVariable(
            reflection.Get(), "BloomConstants", "BloomPadding", 28U, 4U));

        if (std::string_view(entryPoint) == "UpsamplePixel"
            || std::string_view(entryPoint) == "CompositePixel")
        {
            BAFX_CHECK(hasBinding(reflection.Get(), "Source1", D3D_SIT_TEXTURE, 1U));
        }
    }
}
