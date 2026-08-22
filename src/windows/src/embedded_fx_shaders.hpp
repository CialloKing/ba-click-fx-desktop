#pragma once

#include <array>
#include <cstddef>
#include <string>
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
    float globalOpacity : TEXCOORD5;
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
    float globalOpacity : TEXCOORD5;
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
    output.globalOpacity = input.globalOpacity;
    return output;
}

MaterialOutput CrossPixel(PixelInput input)
{
    const float4 sampleValue = MaterialTexture.Sample(MaterialSampler, input.uv);
    const float shape = sampleValue.r;
    const float globalOpacity = saturate(input.globalOpacity);
    const float3 emission = sampleValue.rgb
        * input.color.rgb
        * input.intensity
        * shape
        * globalOpacity;
    const float coverage = saturate(shape * input.color.a)
        * globalOpacity;

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
    const float materialCoverage = sampleValue.a * input.color.a;
    clip(materialCoverage - input.dissolveThreshold);
    const float globalOpacity = saturate(input.globalOpacity);
    const float coverage = materialCoverage * globalOpacity;
    const float3 emission = sampleValue.rgb
        * input.color.rgb
        * input.intensity
        * materialCoverage
        * globalOpacity;

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
    const float materialCoverage = sampleValue.a * input.color.a;
    const float globalOpacity = saturate(input.globalOpacity);
    const float coverage = materialCoverage * globalOpacity;
    const float3 emission = sampleValue.rgb
        * input.color.rgb
        * input.intensity
        * materialCoverage
        * globalOpacity;

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
    const float materialCoverage = saturate(
        sampleValue.a
        * particleOpacity
        * saturate(input.coverageFactor)
        * geometryCoverage);
    const float globalOpacity = saturate(input.globalOpacity);
    const float coverage = materialCoverage * globalOpacity;
    // Desktop Coverage is transport metadata. Unity emission must not be dimmed by it.
    const float3 emission = sampleValue.rgb
        * input.color.rgb
        * input.intensity
        * particleOpacity
        * globalOpacity;

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

namespace detail
{

inline constexpr std::array<std::string_view, 4> unityBloomShaderSourceChunks{
    R"hlsl(
cbuffer BloomConstants : register(b0)
{
    float2 SourceTexelSize;
    float SampleScale;
    float ExposureGain;
    float Threshold;
    float Knee;
    float ClampValue;
    float BackgroundTransportEnabled;
    float BackgroundReferenceWhiteScale;
    float OutputReferenceWhiteScale;
    float ThemeCoverageScale;
    float Padding;
};

Texture2D<float4> Source0 : register(t0);
Texture2D<float4> Source1 : register(t1);
Texture2D<float4> Source2 : register(t2);
Texture2D<float4> Source3 : register(t3);
Texture2D<float4> Source4 : register(t4);
SamplerState LinearClampSampler : register(s0);

static const float BackgroundReferenceWhitePlateau = 1.0 / 1024.0;
static const float BackgroundReferenceWhiteFilterEnd = 3.0 / 1024.0;

float LinearToSrgbChannel(float value)
{
    const float clampedLinear = saturate(value);
    if (clampedLinear <= 0.0031308)
    {
        return clampedLinear * 12.92;
    }
    return 1.055 * pow(clampedLinear, 1.0 / 2.4) - 0.055;
}

float3 LinearToSrgb(float3 value)
{
    return float3(
        LinearToSrgbChannel(value.r),
        LinearToSrgbChannel(value.g),
        LinearToSrgbChannel(value.b));
}

float SrgbToLinearChannel(float value)
{
    const float srgb = saturate(value);
    if (srgb <= 0.04045)
    {
        return srgb / 12.92;
    }
    return pow((srgb + 0.055) / 1.055, 2.4);
}

float3 SrgbPremultipliedToLinearPremultiplied(
    float3 premultiplied,
    float alpha)
{
    const float3 straight = saturate(
        premultiplied / max(alpha, 0.000001));
    return float3(
        SrgbToLinearChannel(straight.r),
        SrgbToLinearChannel(straight.g),
        SrgbToLinearChannel(straight.b)) * alpha;
}

float3 StabilizeCapturedBackground(float3 sample)
{
    const float referenceWhite = max(
        BackgroundReferenceWhiteScale,
        0.000001);
    const float3 distanceFromReferenceWhite = abs(sample - referenceWhite);
    const float3 referenceWhiteBlend = 1.0 - smoothstep(
        BackgroundReferenceWhitePlateau * referenceWhite,
        BackgroundReferenceWhiteFilterEnd * referenceWhite,
        distanceFromReferenceWhite);
    // WGC can alternate between adjacent FP16 values around the negotiated
    // display white. Scale the narrow filter with that physical scRGB value.
    // Negative scRGB is valid undershoot and must survive background transport.
    return lerp(sample, referenceWhite, referenceWhiteBlend);
}

float3 CapturedBackgroundToWorking(float3 physicalScRgb)
{
    // WGC FP16 samples are physical scRGB, where one unit is 80 nits. Unity's
    // authored values remain relative to the display's negotiated SDR white.
    return physicalScRgb / max(
        BackgroundReferenceWhiteScale,
        0.000001);
}

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
    const float2 topLeft = input.uv
        + SourceTexelSize * float2(-1.0, -1.0);
    const float2 topRight = input.uv
        + SourceTexelSize * float2(1.0, -1.0);
    const float2 bottomLeft = input.uv
        + SourceTexelSize * float2(-1.0, 1.0);
    const float2 bottomRight = input.uv
        + SourceTexelSize * float2(1.0, 1.0);
    const float3 backgroundTopLeft = CapturedBackgroundToWorking(
        StabilizeCapturedBackground(
            Source1.Sample(LinearClampSampler, topLeft).rgb));
    const float3 backgroundTopRight = CapturedBackgroundToWorking(
        StabilizeCapturedBackground(
            Source1.Sample(LinearClampSampler, topRight).rgb));
    const float3 backgroundBottomLeft = CapturedBackgroundToWorking(
        StabilizeCapturedBackground(
            Source1.Sample(LinearClampSampler, bottomLeft).rgb));
    const float3 backgroundBottomRight = CapturedBackgroundToWorking(
        StabilizeCapturedBackground(
            Source1.Sample(LinearClampSampler, bottomRight).rgb));
    const float backgroundOcclusionTopLeft = saturate(
        Source2.Sample(LinearClampSampler, topLeft).g);
    const float backgroundOcclusionTopRight = saturate(
        Source2.Sample(LinearClampSampler, topRight).g);
    const float backgroundOcclusionBottomLeft = saturate(
        Source2.Sample(LinearClampSampler, bottomLeft).g);
    const float backgroundOcclusionBottomRight = saturate(
        Source2.Sample(LinearClampSampler, bottomRight).g);
    const float3 background = 0.25 * (
        backgroundTopLeft
        + backgroundTopRight
        + backgroundBottomLeft
        + backgroundBottomRight);
    const float3 scene = 0.25 * (
        Source0.Sample(LinearClampSampler, topLeft).rgb
        + backgroundTopLeft * (1.0 - backgroundOcclusionTopLeft)
        + Source0.Sample(LinearClampSampler, topRight).rgb
        + backgroundTopRight * (1.0 - backgroundOcclusionTopRight)
        + Source0.Sample(LinearClampSampler, bottomLeft).rgb
        + backgroundBottomLeft * (1.0 - backgroundOcclusionBottomLeft)
        + Source0.Sample(LinearClampSampler, bottomRight).rgb
        + backgroundBottomRight * (1.0 - backgroundOcclusionBottomRight));
    const float3 differential = max(
        BrightPass(scene).rgb - BrightPass(background).rgb,
        0.0);
    const float differentialEnergy = max(
        differential.r,
        max(differential.g, differential.b));
    return float4(differential, differentialEnergy);
}

float4 TemporalBackgroundPixel(FullscreenOutput input) : SV_Target0
{
    const int2 pixel = int2(input.position.xy);
    const float3 previous = StabilizeCapturedBackground(
        Source0.Load(int3(pixel, 0)).rgb);
    const float3 current = StabilizeCapturedBackground(
        Source1.Load(int3(pixel, 0)).rgb);
    const float3 peak = max(previous, current);
    const float luminance = saturate(dot(peak, float3(0.2126, 0.7152, 0.0722)));
    // The WGC FP16 conversion is least stable near a bright reference white.
    // A luminance-scaled deadband filters that noise while preserving larger
    // desktop changes, such as a window crossing the effect.
    const float peakChannel = max(peak.r, max(peak.g, peak.b));
    const float absoluteDeadband = lerp(
        0.5 / 1024.0,
        2.0 / 1024.0,
        luminance);
    const float deadband = max(
        absoluteDeadband,
        peakChannel * (2.0 / 1024.0));
    const float fullResponse = deadband * 4.0;
    const float channelDelta = max(
        abs(current.r - previous.r),
        max(abs(current.g - previous.g), abs(current.b - previous.b)));
    const float response = smoothstep(deadband, fullResponse, channelDelta);
    // Keep the result in the same linear scRGB domain as the Unity transport;
    // no gamma conversion or Alpha adjustment belongs in this history pass.
    return float4(lerp(previous, current, response), 1.0);
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

float4 ResolveBloomResult(FullscreenOutput input)
{
    const float2 offset = SourceTexelSize * (SampleScale * 0.5);
    const float4 bloom = FourTap(Source1, input.uv, offset);
    return float4(
        bloom.rgb * ExposureGain,
        saturate(bloom.a * ExposureGain));
}

float4 ResolveComposite(float4 direct, float4 bloomResult)
{
    return float4(
        direct.rgb + bloomResult.rgb,
        max(direct.a, bloomResult.a));
}

float4 CompositePixel(FullscreenOutput input) : SV_Target0
{
    const float4 direct = Source0.Sample(LinearClampSampler, input.uv);
    return ResolveComposite(direct, ResolveBloomResult(input));
}

float4 BloomResultPixel(FullscreenOutput input) : SV_Target0
{
    return ResolveBloomResult(input);
}
)hlsl",
    R"hlsl(
struct CaptureCompositeOutput
{
    float4 finalOverlay : SV_Target0;
    float4 bloomResult : SV_Target1;
};

CaptureCompositeOutput CaptureCompositePixel(FullscreenOutput input)
{
    const float4 direct = Source0.Sample(LinearClampSampler, input.uv);
    const float4 bloomResult = ResolveBloomResult(input);

    CaptureCompositeOutput output;
    output.finalOverlay = ResolveComposite(direct, bloomResult);
    output.bloomResult = bloomResult;
    return output;
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
    const float alpha = min(
        ThemeCoverageScale >= 0.999999
            ? transportCapacity
            : transportCapacity * saturate(ThemeCoverageScale),
        overlayAlphaLimit);

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

float4 ResolveUnknownBackgroundDesktopTransport(
    float4 direct,
    float4 bloom,
    float exposureGain,
    float alphaLimit,
    float compensationMix)
{
    const float3 linearRgb = max(direct.rgb, 0.0)
        + max(bloom.rgb, 0.0) * exposureGain;
    const float3 srgbRgb = LinearToSrgb(linearRgb);
    const float sceneCoverage = saturate(direct.a);
    const float bloomTransport = LinearToSrgbChannel(
        max(bloom.a, 0.0) * exposureGain);
    // Unknown-background source-over follows the Web transparent-overlay
    // contract: visual-max chooses the larger independent envelope instead of
    // summing Alpha, then the selected preset applies its explicit cap.
    const float alpha = min(
        ThemeCoverageScale >= 0.999999
            ? max(sceneCoverage, bloomTransport)
            : max(sceneCoverage, bloomTransport) * saturate(ThemeCoverageScale),
        alphaLimit);
    if (alpha <= 0.000001)
    {
        return float4(0.0, 0.0, 0.0, 0.0);
    }

    const float maximumSrgb = max(
        srgbRgb.r,
        max(srgbRgb.g, srgbRgb.b));
    const float capacityScale = min(
        1.0,
        alpha / max(maximumSrgb, 0.000001));
    float3 premultipliedSrgb = srgbRgb * capacityScale;

    const float maximumPremultiplied = max(
        premultipliedSrgb.r,
        max(premultipliedSrgb.g, premultipliedSrgb.b));
    const float energyRatio = maximumPremultiplied
        / max(alpha, 0.000001);
    const float gate = smoothstep(0.25, 0.75, energyRatio)
        * smoothstep(0.03125, 0.25, maximumPremultiplied);

    // bright-core only lifts weaker channels toward the existing peak. It
    // never raises peak energy or turns the low-energy trail tail gray-white.
    premultipliedSrgb = lerp(
        premultipliedSrgb,
        maximumPremultiplied.xxx,
        compensationMix * gate);

    // Chromium transports the transparent Canvas as premultiplied sRGB. DComp
    // consumes linear scRGB, so preserve the same straight color by
    // unpremultiplying before the transfer conversion and premultiplying again.
    const float3 premultipliedLinear =
        SrgbPremultipliedToLinearPremultiplied(
            min(premultipliedSrgb, alpha),
            alpha);
    return float4(min(premultipliedLinear, alpha), alpha);
}

float4 ResolveRecordingCompatibleDesktopTransport(
    float4 direct,
    float4 bloom,
    float exposureGain)
{
    // Match the browser preset shown in the transparent-overlay controls:
    // visual-max + bright-core with a 0.90 source-over Alpha ceiling.
    return ResolveUnknownBackgroundDesktopTransport(
        direct,
        bloom,
        exposureGain,
        0.90,
        0.35);
}

float4 ResolveLightBackgroundDesktopTransport(
    float4 direct,
    float4 bloom,
    float exposureGain)
{
    // Keep the dedicated light-background preset slightly more conservative
    // on ordinary white surfaces than recording-compatible fitting.
    return ResolveUnknownBackgroundDesktopTransport(
        direct,
        bloom,
        exposureGain,
        0.85,
        0.35);
}
)hlsl",
    R"hlsl(
float4 ResolveBackgroundAwareDesktopTransport(
    float4 direct,
    float4 bloom,
    float occlusion,
    float3 capturedBackground,
    float exposureGain,
    float outputReferenceWhiteScale,
    float backgroundOutputScale)
{
    // Preserve continuous scRGB outside a narrow reference-white noise band.
    const float3 background = StabilizeCapturedBackground(capturedBackground)
        * backgroundOutputScale;

    // Keep Alpha tied to the authored Coverage/Bloom envelope used by the Web
    // coverage path. WGC is asynchronous to DWM, so inverse-solving Alpha from
    // captured RGB is ill-conditioned on a light desktop.
    const float sceneCoverage = saturate(direct.a);
    const float crossCoverage = saturate(occlusion);
    const float additiveCoverage = saturate(
        (sceneCoverage - crossCoverage)
        / max(1.0 - crossCoverage, 0.000001));
    const float bloomTransport = max(bloom.a, 0.0) * exposureGain;
    const float requestedCoverage = saturate(
        crossCoverage + additiveCoverage + bloomTransport);
    // Additive trail emission is intentionally independent from Coverage.
    // It is fitted into the authored transport envelope in RGB below; letting
    // its peak energy raise Alpha makes a fading tail pulse on light surfaces.
    const float transportCapacity = saturate(requestedCoverage);
    const float overlayAlphaLimit = 250.0 / 255.0;

    // Cross2 occlusion and additive/Bloom transport already provide the
    // authored source-over envelope. Keep this Alpha independent from the
    // captured RGB difference: inverse-solving it would make a fading trail
    // pulse whenever WGC reports a neighboring light-background sample.
    const float authoredAlpha = min(transportCapacity, overlayAlphaLimit);
    const float alpha = authoredAlpha;
    if (alpha <= 0.000001)
    {
        return float4(0.0, 0.0, 0.0, 0.0);
    }
    // Keep direct emission and Bloom additive, matching the Web default
    // `coverage` policy. Only the Cross2 background attenuation is represented
    // by the source-over term; it must not scale an independent trail RGB peak
    // back down to its Coverage Alpha.
    const float3 additiveEmission = (
        max(direct.rgb, 0.0)
        + max(bloom.rgb, 0.0) * exposureGain) * outputReferenceWhiteScale;
    const float3 backgroundCoveragePayload = background * max(
        alpha - crossCoverage,
        0.0);
    const float3 premultiplied = additiveEmission + backgroundCoveragePayload;

    // The captured background is baked into the payload only as required to
    // reproduce Unity's target. Extended premultiplied transport intentionally
    // permits additive RGB to exceed Coverage Alpha. Keep its signed physical
    // scRGB values intact; only relative FX emission is mapped to output white.
    return float4(premultiplied, alpha);
}

float4 ScaleFxForOutput(float4 relativeFx)
{
    return float4(relativeFx.rgb * OutputReferenceWhiteScale, relativeFx.a);
}

float4 EncodeConservativeSdrPremultiplied(float4 linearPremultiplied)
{
    const float alpha = saturate(linearPremultiplied.a);
    if (alpha <= 0.000001)
    {
        return float4(0.0, 0.0, 0.0, 0.0);
    }

    // This compatibility path intentionally reserves RGB within Alpha and the
    // SDR range. Spout uses the extended encoder when additive RGB must exceed
    // geometric coverage.
    const float3 straightLinear = saturate(
        max(linearPremultiplied.rgb, 0.0) / alpha);
    const float3 encodedStraight = LinearToSrgb(straightLinear);
    const float3 encodedPremultiplied = min(
        encodedStraight * alpha,
        alpha);
    return float4(encodedPremultiplied, alpha);
}

float4 ResolveSpout2FxOnlyTransport(
    float4 direct,
    float4 bloom,
    float4 cross,
    float exposureGain)
{
    const float crossCoverage = saturate(cross.a);
    const float3 emission = max(direct.rgb, 0.0)
        + max(bloom.rgb, 0.0) * exposureGain;
    const float emissionMaximum = max(
        emission.r,
        max(emission.g, emission.b));
    const float coverageScale = saturate(ThemeCoverageScale);
    const float overlayAlphaLimit = 250.0 / 255.0;
    const float crossAlpha = min(
        crossCoverage * coverageScale,
        overlayAlphaLimit);
    // Cross2 is the only material that attenuates the OBS background. Keep a
    // sub-UNORM sentinel for additive-only pixels; the encoder promotes it to
    // one stored Alpha step only when an RGB byte survives quantization.
    const float additiveAlphaSentinel = emissionMaximum > 0.0
            && coverageScale > 0.0
        ? 0.000001
        : 0.0;
    const float alpha = max(crossAlpha, additiveAlphaSentinel);

    // Extended RGB preserves the authored additive energy. The near-zero
    // additive Alpha avoids receiver canonicalization without visibly
    // darkening the independently captured OBS background.
    return float4(emission, alpha);
}
)hlsl",
R"hlsl(

float3 EncodeObsSdrAdditiveDelta(float3 linearEmission)
{
    const float3 emission = max(linearEmission, 0.0);
    const float peak = max(emission.r, max(emission.g, emission.b));
    // The Spout2 OBS source adds stored bytes to an already encoded game
    // frame. Applying the sRGB OETF to the isolated emission would brighten
    // low-energy Bloom a second time. A shared shoulder keeps hue and retains
    // highlight detail without requiring access to OBS's background texture.
    return emission / (1.0 + peak);
}

float4 EncodeObsSdrExtendedPremultiplied(float4 linearExtendedPremultiplied)
{
    // BGRA8 clamps SDR channel range, but it can still carry RGB above Alpha.
    // Keep one stored Alpha step wherever an encoded RGB byte can survive.
    // Some Spout receivers canonicalize RGB to black when the Alpha byte is 0.
    const float3 encoded = EncodeObsSdrAdditiveDelta(
        linearExtendedPremultiplied.rgb);
    const float storedByteThreshold = 0.5 / 255.0;
    const float minimumStoredAlpha = max(
            encoded.r,
            max(encoded.g, encoded.b)) >= storedByteThreshold
        && linearExtendedPremultiplied.a > 0.0
        ? 1.0 / 255.0
        : 0.0;
    return float4(
        encoded,
        max(saturate(linearExtendedPremultiplied.a), minimumStoredAlpha));
}

float4 ResolveDesktopCompositeInputs(
    FullscreenOutput input,
    float4 direct,
    float4 bloom,
    float exposureGain,
    float backgroundOutputScale)
{
    float4 resolved = float4(0.0, 0.0, 0.0, 0.0);
    if (BackgroundTransportEnabled <= 0.0)
    {
        const float4 cross = Source4.Sample(
            LinearClampSampler,
            input.uv);
        resolved = ScaleFxForOutput(
            ResolveFxOnlyDesktopTransport(
                direct,
                bloom,
                cross,
                exposureGain));
    }
    else
    {
        const float occlusion = Source2.Sample(
            LinearClampSampler,
            input.uv).r;
        const float3 background = Source3.Sample(
            LinearClampSampler,
            input.uv).rgb;
        // The render owner latches one complete visual path. Differential Bloom
        // and the background payload arrive together so capture cadence cannot
        // switch either layer independently.
        resolved = ResolveBackgroundAwareDesktopTransport(
            direct,
            bloom,
            occlusion,
            background,
            exposureGain,
            OutputReferenceWhiteScale,
            backgroundOutputScale);
    }
    return resolved;
}

float4 ResolveDesktopComposite(
    FullscreenOutput input,
    float backgroundOutputScale)
{
    const float4 direct = Source0.Sample(LinearClampSampler, input.uv);
    const float2 offset = SourceTexelSize * (SampleScale * 0.5);
    const float4 bloom = FourTap(Source1, input.uv, offset);
    return ResolveDesktopCompositeInputs(
        input,
        direct,
        bloom,
        ExposureGain,
        backgroundOutputScale);
}

float4 DesktopCompositePixel(FullscreenOutput input) : SV_Target0
{
    return ResolveDesktopComposite(input, 1.0);
}

float4 DesktopSdrCompositePixel(FullscreenOutput input) : SV_Target0
{
    return EncodeConservativeSdrPremultiplied(
        ResolveDesktopComposite(
            input,
            1.0 / max(BackgroundReferenceWhiteScale, 0.000001)));
}

struct DesktopCaptureCompositeOutput
{
    float4 finalOverlay : SV_Target0;
    float4 bloomResult : SV_Target1;
};

DesktopCaptureCompositeOutput DesktopCaptureCompositePixel(
    FullscreenOutput input)
{
    const float4 direct = Source0.Sample(LinearClampSampler, input.uv);
    const float2 offset = SourceTexelSize * (SampleScale * 0.5);
    const float4 bloom = FourTap(Source1, input.uv, offset);

    DesktopCaptureCompositeOutput output;
    output.finalOverlay = ResolveDesktopCompositeInputs(
        input,
        direct,
        bloom,
        ExposureGain,
        1.0);
    output.bloomResult = float4(
        bloom.rgb * ExposureGain,
        saturate(bloom.a * ExposureGain));
    return output;
}

DesktopCaptureCompositeOutput DesktopCaptureSdrCompositePixel(
    FullscreenOutput input)
{
    DesktopCaptureCompositeOutput output = DesktopCaptureCompositePixel(input);
    output.finalOverlay = EncodeConservativeSdrPremultiplied(
        ResolveDesktopComposite(
            input,
            1.0 / max(BackgroundReferenceWhiteScale, 0.000001)));
    return output;
}

float4 ResolveLightBackgroundComposite(FullscreenOutput input)
{
    const float4 direct = Source0.Sample(LinearClampSampler, input.uv);
    const float2 offset = SourceTexelSize * (SampleScale * 0.5);
    const float4 bloom = FourTap(Source1, input.uv, offset);
    return ScaleFxForOutput(
        ResolveLightBackgroundDesktopTransport(
            direct,
            bloom,
            ExposureGain));
}

float4 LightBackgroundCompositePixel(FullscreenOutput input) : SV_Target0
{
    return ResolveLightBackgroundComposite(input);
}

float4 LightBackgroundSdrCompositePixel(FullscreenOutput input) : SV_Target0
{
    return EncodeConservativeSdrPremultiplied(
        ResolveLightBackgroundComposite(input));
}

float4 ResolveRecordingCompatibleComposite(FullscreenOutput input)
{
    const float4 direct = Source0.Sample(LinearClampSampler, input.uv);
    const float2 offset = SourceTexelSize * (SampleScale * 0.5);
    const float4 bloom = FourTap(Source1, input.uv, offset);
    return ScaleFxForOutput(
        ResolveRecordingCompatibleDesktopTransport(
            direct,
            bloom,
            ExposureGain));
}

float4 RecordingCompatibleCompositePixel(FullscreenOutput input) : SV_Target0
{
    return ResolveRecordingCompatibleComposite(input);
}

float4 RecordingCompatibleSdrCompositePixel(FullscreenOutput input) : SV_Target0
{
    return EncodeConservativeSdrPremultiplied(
        ResolveRecordingCompatibleComposite(input));
}

float4 RecordingFxOnlySdrCompositePixel(FullscreenOutput input) : SV_Target0
{
    const float4 direct = Source0.Sample(LinearClampSampler, input.uv);
    const float4 bloom = Source1.Sample(LinearClampSampler, input.uv);
    const float4 cross = Source2.Sample(LinearClampSampler, input.uv);
    const float4 overlay = ResolveSpout2FxOnlyTransport(
        direct,
        bloom,
        cross,
        1.0);
    return EncodeObsSdrExtendedPremultiplied(overlay);
}

// Core mode deliberately omits Bloom and background transport. The direct
// material surface is already the complete low-cost FX payload.
float4 CoreCompositePixel(FullscreenOutput input) : SV_Target0
{
    const float4 direct = Source0.Sample(LinearClampSampler, input.uv);
    if (ThemeCoverageScale >= 0.999999)
    {
        return EncodeConservativeSdrPremultiplied(direct);
    }
    const float alpha = saturate(direct.a * ThemeCoverageScale);
    return EncodeConservativeSdrPremultiplied(
        float4(min(max(direct.rgb, 0.0), alpha), alpha));
}

float4 CoreRecordingFxOnlySdrCompositePixel(FullscreenOutput input) : SV_Target0
{
    const float4 direct = Source0.Sample(LinearClampSampler, input.uv);
    const float4 cross = Source2.Sample(LinearClampSampler, input.uv);
    return EncodeObsSdrExtendedPremultiplied(
        ResolveSpout2FxOnlyTransport(
            direct,
            float4(0.0, 0.0, 0.0, 0.0),
            cross,
            0.0));
}
)hlsl"};

// Older MSVC front ends reject individual string literals near 16 KiB before
// semantic analysis. Keep headroom so the embedded shader remains cross-SDK.
inline constexpr std::size_t maximumEmbeddedShaderChunkSize = 12U * 1024U;
static_assert(unityBloomShaderSourceChunks[0].size() < maximumEmbeddedShaderChunkSize);
static_assert(unityBloomShaderSourceChunks[1].size() < maximumEmbeddedShaderChunkSize);
static_assert(unityBloomShaderSourceChunks[2].size() < maximumEmbeddedShaderChunkSize);
static_assert(unityBloomShaderSourceChunks[3].size() < maximumEmbeddedShaderChunkSize);

inline constexpr std::size_t unityBloomShaderSourceSize =
    unityBloomShaderSourceChunks[0].size()
    + unityBloomShaderSourceChunks[1].size()
    + unityBloomShaderSourceChunks[2].size()
    + unityBloomShaderSourceChunks[3].size();

}

[[nodiscard]] inline std::string_view unityBloomShaderSource()
{
    static const std::string source = []()
    {
        std::string result;
        result.reserve(detail::unityBloomShaderSourceSize);
        for (const std::string_view chunk : detail::unityBloomShaderSourceChunks)
        {
            result.append(chunk.data(), chunk.size());
        }
        return result;
    }();
    return std::string_view(source.data(), source.size());
}

}
