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
