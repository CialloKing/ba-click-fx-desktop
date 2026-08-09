#pragma once

#include <string_view>

namespace bafx::windows
{

inline constexpr std::string_view fxMaterialsShaderSource = R"hlsl(
cbuffer ViewportConstants : register(b0)
{
    float2 ViewportSize;
    float2 ViewportPadding;
};

Texture2D<float4> MaterialTexture : register(t0);
SamplerState MaterialSampler : register(s0);

struct VertexInput
{
    float2 positionPixels : POSITION;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
    float intensity : TEXCOORD1;
    float dissolveThreshold : TEXCOORD2;
    float bloomEnabled : TEXCOORD3;
};

struct PixelInput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
    float intensity : TEXCOORD1;
    float dissolveThreshold : TEXCOORD2;
    float bloomEnabled : TEXCOORD3;
};

struct MaterialOutput
{
    float4 direct : SV_Target0;
    float4 bloomSeed : SV_Target1;
};

PixelInput SpriteVertex(VertexInput input)
{
    PixelInput output;
    const float2 normalized = input.positionPixels / ViewportSize;
    output.position = float4(
        normalized.x * 2.0 - 1.0,
        1.0 - normalized.y * 2.0,
        0.0,
        1.0);
    output.uv = input.uv;
    output.color = input.color;
    output.intensity = input.intensity;
    output.dissolveThreshold = input.dissolveThreshold;
    output.bloomEnabled = input.bloomEnabled;
    return output;
}

MaterialOutput CrossPixel(PixelInput input)
{
    const float4 sampleValue = MaterialTexture.Sample(MaterialSampler, input.uv);
    const float shape = sampleValue.r;
    const float3 emission = sampleValue.rgb
        * input.color.rgb
        * input.intensity
        * shape;
    const float coverage = saturate(shape * input.color.a);

    MaterialOutput output;
    output.direct = float4(emission, coverage);
    output.bloomSeed = float4(emission * input.bloomEnabled, 0.0);
    return output;
}

MaterialOutput DissolvePixel(PixelInput input)
{
    const float4 sampleValue = MaterialTexture.Sample(MaterialSampler, input.uv);
    const float coverage = sampleValue.a * input.color.a;
    clip(coverage - input.dissolveThreshold);
    const float3 emission = sampleValue.rgb
        * input.color.rgb
        * input.intensity
        * coverage;

    MaterialOutput output;
    output.direct = float4(emission, 0.0);
    output.bloomSeed = float4(emission * input.bloomEnabled, 0.0);
    return output;
}

MaterialOutput AdditivePixel(PixelInput input)
{
    const float4 sampleValue = MaterialTexture.Sample(MaterialSampler, input.uv);
    const float coverage = sampleValue.a * input.color.a;
    const float3 emission = sampleValue.rgb
        * input.color.rgb
        * input.intensity
        * coverage;

    MaterialOutput output;
    output.direct = float4(emission, 0.0);
    output.bloomSeed = float4(emission * input.bloomEnabled, 0.0);
    return output;
}
)hlsl";

inline constexpr std::string_view unityBloomShaderSource = R"hlsl(
cbuffer BloomConstants : register(b0)
{
    float2 SourceTexelSize;
    float SampleScale;
    float ExposureGain;
    float Threshold;
    float Knee;
    float ClampValue;
    float BloomPadding;
};

Texture2D<float4> Source0 : register(t0);
Texture2D<float4> Source1 : register(t1);
SamplerState LinearClampSampler : register(s0);

struct FullscreenOutput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

FullscreenOutput FullscreenVertex(uint vertexId : SV_VertexID)
{
    FullscreenOutput output;
    output.uv = float2((vertexId << 1U) & 2U, vertexId & 2U);
    output.position = float4(
        output.uv.x * 2.0 - 1.0,
        1.0 - output.uv.y * 2.0,
        0.0,
        1.0);
    return output;
}

float4 FourTap(Texture2D<float4> source, float2 uv, float2 offset)
{
    return 0.25 * (
        source.Sample(LinearClampSampler, uv + offset * float2(-1.0, -1.0)) +
        source.Sample(LinearClampSampler, uv + offset * float2(1.0, -1.0)) +
        source.Sample(LinearClampSampler, uv + offset * float2(-1.0, 1.0)) +
        source.Sample(LinearClampSampler, uv + offset * float2(1.0, 1.0)));
}

float4 PrefilterPixel(FullscreenOutput input) : SV_Target0
{
    float3 color = FourTap(Source0, input.uv, SourceTexelSize).rgb;
    color = min(max(color, 0.0), ClampValue);
    const float brightness = max(color.r, max(color.g, color.b));
    float soft = clamp(brightness - (Threshold - Knee), 0.0, 2.0 * Knee);
    soft = soft * soft * (0.25 / Knee);
    const float contribution = max(soft, brightness - Threshold)
        / max(brightness, 0.0001);
    return float4(color * contribution, 0.0);
}

float4 DownsamplePixel(FullscreenOutput input) : SV_Target0
{
    return float4(FourTap(Source0, input.uv, SourceTexelSize).rgb, 0.0);
}

float4 UpsamplePixel(FullscreenOutput input) : SV_Target0
{
    const float2 offset = SourceTexelSize * (SampleScale * 0.5);
    const float3 coarse = FourTap(Source0, input.uv, offset).rgb;
    const float3 currentFine = Source1.Sample(LinearClampSampler, input.uv).rgb;
    return float4(coarse + currentFine, 0.0);
}

float4 CompositePixel(FullscreenOutput input) : SV_Target0
{
    const float4 direct = Source0.Sample(LinearClampSampler, input.uv);
    const float2 offset = SourceTexelSize * (SampleScale * 0.5);
    const float3 bloom = FourTap(Source1, input.uv, offset).rgb;
    return float4(direct.rgb + bloom * ExposureGain, direct.a);
}
)hlsl";

}
