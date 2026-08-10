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
    float coverageFactor : TEXCOORD4;
};

struct PixelInput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
    float intensity : TEXCOORD1;
    float dissolveThreshold : TEXCOORD2;
    float bloomEnabled : TEXCOORD3;
    float coverageFactor : TEXCOORD4;
};

struct MaterialOutput
{
    float4 direct : SV_Target0;
    float4 bloomSeed : SV_Target1;
    float2 occlusion : SV_Target2;
    float4 cross : SV_Target3;
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
    output.coverageFactor = input.coverageFactor;
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
    output.bloomSeed = float4(
        emission * input.bloomEnabled,
        coverage * input.bloomEnabled);
    output.occlusion = float2(
        coverage,
        coverage * input.bloomEnabled);
    output.cross = output.direct;
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
    output.direct = float4(emission, coverage);
    output.bloomSeed = float4(
        emission * input.bloomEnabled,
        coverage * input.bloomEnabled);
    output.occlusion = float2(0.0, 0.0);
    output.cross = float4(0.0, 0.0, 0.0, 0.0);
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
    output.direct = float4(emission, coverage);
    output.bloomSeed = float4(
        emission * input.bloomEnabled,
        coverage * input.bloomEnabled);
    output.occlusion = float2(0.0, 0.0);
    output.cross = float4(0.0, 0.0, 0.0, 0.0);
    return output;
}

MaterialOutput TrailPixel(PixelInput input)
{
    const float4 sampleValue = MaterialTexture.Sample(MaterialSampler, input.uv);
    const float particleOpacity = saturate(input.color.a);
    const float edgeDistance = min(input.uv.y, 1.0 - input.uv.y);
    const float footprint = max(fwidth(input.uv.y) * 0.5, 0.000001);
    const float geometryCoverage = smoothstep(0.0, footprint, edgeDistance);
    const float coverage = saturate(
        sampleValue.a
        * particleOpacity
        * saturate(input.coverageFactor)
        * geometryCoverage);
    // Desktop Coverage is transport metadata. Unity emission must not be dimmed by it.
    const float3 emission = sampleValue.rgb
        * input.color.rgb
        * input.intensity
        * particleOpacity;

    MaterialOutput output;
    output.direct = float4(emission, coverage);
    output.bloomSeed = float4(
        emission * input.bloomEnabled,
        coverage * input.bloomEnabled);
    output.occlusion = float2(0.0, 0.0);
    output.cross = float4(0.0, 0.0, 0.0, 0.0);
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
    float BackgroundTransportEnabled;
};

Texture2D<float4> Source0 : register(t0);
Texture2D<float4> Source1 : register(t1);
Texture2D<float4> Source2 : register(t2);
Texture2D<float4> Source3 : register(t3);
Texture2D<float4> Source4 : register(t4);
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

float4 BrightPass(float3 source)
{
    float3 color = source;
    color = min(max(color, 0.0), ClampValue);
    const float brightness = max(color.r, max(color.g, color.b));
    float soft = clamp(brightness - (Threshold - Knee), 0.0, 2.0 * Knee);
    soft = soft * soft * (0.25 / Knee);
    const float contribution = max(soft, brightness - Threshold)
        / max(brightness, 0.0001);
    const float transportEnergy = brightness * contribution;

    // Alpha carries an upper bound for every bright-pass RGB channel. Keeping
    // it independent from geometric coverage lets the final pass reserve just
    // enough premultiplied capacity for Bloom on an unknown desktop background.
    return float4(color * contribution, transportEnergy);
}

float4 PrefilterPixel(FullscreenOutput input) : SV_Target0
{
    const float4 source = FourTap(Source0, input.uv, SourceTexelSize);
    return BrightPass(source.rgb);
}

float4 DifferentialPrefilterPixel(FullscreenOutput input) : SV_Target0
{
    const float3 background = FourTap(Source1, input.uv, SourceTexelSize).rgb;
    const float2 topLeft = input.uv
        + SourceTexelSize * float2(-1.0, -1.0);
    const float2 topRight = input.uv
        + SourceTexelSize * float2(1.0, -1.0);
    const float2 bottomLeft = input.uv
        + SourceTexelSize * float2(-1.0, 1.0);
    const float2 bottomRight = input.uv
        + SourceTexelSize * float2(1.0, 1.0);
    const float3 scene = 0.25 * (
        Source0.Sample(LinearClampSampler, topLeft).rgb
            + Source1.Sample(LinearClampSampler, topLeft).rgb
                * (1.0 - saturate(
                    Source2.Sample(LinearClampSampler, topLeft).g))
        + Source0.Sample(LinearClampSampler, topRight).rgb
            + Source1.Sample(LinearClampSampler, topRight).rgb
                * (1.0 - saturate(
                    Source2.Sample(LinearClampSampler, topRight).g))
        + Source0.Sample(LinearClampSampler, bottomLeft).rgb
            + Source1.Sample(LinearClampSampler, bottomLeft).rgb
                * (1.0 - saturate(
                    Source2.Sample(LinearClampSampler, bottomLeft).g))
        + Source0.Sample(LinearClampSampler, bottomRight).rgb
            + Source1.Sample(LinearClampSampler, bottomRight).rgb
                * (1.0 - saturate(
                    Source2.Sample(LinearClampSampler, bottomRight).g)));
    const float3 differential = max(
        BrightPass(scene).rgb - BrightPass(background).rgb,
        0.0);
    const float transportEnergy = max(
        differential.r,
        max(differential.g, differential.b));
    return float4(differential, transportEnergy);
}

float4 DownsamplePixel(FullscreenOutput input) : SV_Target0
{
    return FourTap(Source0, input.uv, SourceTexelSize);
}

float4 UpsamplePixel(FullscreenOutput input) : SV_Target0
{
    const float2 offset = SourceTexelSize * (SampleScale * 0.5);
    const float4 coarse = FourTap(Source0, input.uv, offset);
    const float4 currentFine = Source1.Sample(LinearClampSampler, input.uv);
    return coarse + currentFine;
}

float4 CompositePixel(FullscreenOutput input) : SV_Target0
{
    const float4 direct = Source0.Sample(LinearClampSampler, input.uv);
    const float2 offset = SourceTexelSize * (SampleScale * 0.5);
    const float4 bloom = FourTap(Source1, input.uv, offset);
    const float bloomCoverage = saturate(bloom.a * ExposureGain);
    return float4(
        direct.rgb + bloom.rgb * ExposureGain,
        max(direct.a, bloomCoverage));
}

float4 ResolveFxOnlyDesktopTransport(
    float4 direct,
    float4 bloom,
    float4 cross,
    float exposureGain)
{
    const float crossCoverage = saturate(cross.a);
    const float sceneCoverage = saturate(direct.a);
    // The direct target stores a source-over union. Remove Cross2's share and
    // invert that union so additive capacity remains independent from it.
    const float additiveCoverage = saturate(
        (sceneCoverage - crossCoverage)
        / max(1.0 - crossCoverage, 0.000001));
    const float bloomTransport = max(bloom.a, 0.0) * exposureGain;
    const float residualCapacity = additiveCoverage + bloomTransport;
    const float requestedCapacity = crossCoverage + residualCapacity;
    const float transportCapacity = saturate(requestedCapacity);
    const float overlayAlphaLimit = 250.0 / 255.0;
    const float alpha = min(transportCapacity, overlayAlphaLimit);

    if (alpha <= 0.000001)
    {
        return float4(0.0, 0.0, 0.0, 0.0);
    }

    const float3 crossRgb = max(cross.rgb, 0.0);
    const float crossMaximum = max(
        crossRgb.r,
        max(crossRgb.g, crossRgb.b));
    const float crossScale = min(
        1.0,
        crossCoverage / max(crossMaximum, 0.000001));
    const float3 crossPayload = crossRgb * crossScale;

    const float3 additiveRgb = max(direct.rgb - cross.rgb, 0.0);
    const float3 residualRgb = additiveRgb
        + max(bloom.rgb, 0.0) * exposureGain;
    const float residualMaximum = max(
        residualRgb.r,
        max(residualRgb.g, residualRgb.b));
    const float residualScale = min(
        1.0,
        residualCapacity / max(residualMaximum, 0.000001));
    const float3 residualPayload = residualRgb * residualScale;

    // Cross2 owns the background-attenuating disk. Reserve its premultiplied
    // capacity before additive materials so their pulsing Alpha cannot dim the
    // stable disk when WGC temporarily falls back to FX-only transport.
    const float crossTransportCapacity = min(crossCoverage, alpha);
    const float crossTransportScale = min(
        1.0,
        crossTransportCapacity / max(crossCoverage, 0.000001));
    const float availableResidualCapacity = max(
        alpha - crossTransportCapacity,
        0.0);
    const float residualTransportScale = min(
        1.0,
        availableResidualCapacity / max(residualCapacity, 0.000001));
    const float3 linearRgb = crossPayload * crossTransportScale
        + residualPayload * residualTransportScale;

    // The DComp swap chain declares premultiplied Alpha. Converging RGB here
    // preserves each authored layer's capacity while preventing an opaque or
    // dark payload from being exported when the desktop is not sampled.
    return float4(
        min(linearRgb, alpha),
        alpha);
}

float SolveOverlayAlpha(float background, float target)
{
    if (target > background)
    {
        return (target - background) / max(1.0 - background, 0.000001);
    }
    if (target < background)
    {
        return (background - target) / max(background, 0.000001);
    }
    return 0.0;
}

float4 ResolveBackgroundAwareDesktopTransport(
    float4 direct,
    float4 bloom,
    float occlusion,
    float3 capturedBackground,
    float exposureGain)
{
    const float3 background = saturate(capturedBackground);
    const float3 target = saturate(
        max(direct.rgb, 0.0)
        + background * (1.0 - saturate(occlusion))
        + max(bloom.rgb, 0.0) * exposureGain);
    const float3 difference = abs(target - background);
    if (max(difference.r, max(difference.g, difference.b)) <= 0.000001)
    {
        return float4(0.0, 0.0, 0.0, 0.0);
    }

    const float3 channelAlpha = float3(
        SolveOverlayAlpha(background.r, target.r),
        SolveOverlayAlpha(background.g, target.g),
        SolveOverlayAlpha(background.b, target.b));
    const float alpha = saturate(max(
        channelAlpha.r,
        max(channelAlpha.g, channelAlpha.b)));
    const float3 premultiplied = target - background * (1.0 - alpha);

    // The captured background is baked into the payload only as required to
    // reproduce Unity's target. DWM still receives a valid premultiplied layer.
    return float4(clamp(premultiplied, 0.0, alpha), alpha);
}

float4 DesktopCompositePixel(FullscreenOutput input) : SV_Target0
{
    const float4 direct = Source0.Sample(LinearClampSampler, input.uv);
    const float2 offset = SourceTexelSize * (SampleScale * 0.5);
    const float4 bloom = FourTap(Source1, input.uv, offset);
    if (BackgroundTransportEnabled <= 0.0)
    {
        const float4 cross = Source4.Sample(
            LinearClampSampler,
            input.uv);
        return ResolveFxOnlyDesktopTransport(
            direct,
            bloom,
            cross,
            ExposureGain);
    }

    const float occlusion = Source2.Sample(
        LinearClampSampler,
        input.uv).r;
    const float3 background = Source3.Sample(
        LinearClampSampler,
        input.uv).rgb;
    // The render owner latches one complete visual path. Differential Bloom
    // and Alpha reconstruction arrive together so capture cadence cannot
    // modulate either layer independently.
    return ResolveBackgroundAwareDesktopTransport(
        direct,
        bloom,
        occlusion,
        background,
        ExposureGain);
}
)hlsl";

}
