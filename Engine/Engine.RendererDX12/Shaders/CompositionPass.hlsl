#include "CBufferStructures.hlsl"
#include "FullScreenVS.hlsl"

struct CompositionConstants
{
    uint SSAAMultiplier;
};

ConstantBuffer<CompositionConstants> CBComposition : register(b0);

SamplerState samPointWrap : register(s0);
SamplerState samPointClamp : register(s1);
SamplerState samLinearWrap : register(s2);
SamplerState samLinearClamp : register(s3);
SamplerState samAnisotropicWrap : register(s4);
SamplerState samAnisotropicClamp : register(s5);

Texture2D<float4> OpaqueTexture : register(t0);
Texture2D<float4> AccumTexture : register(t1);
Texture2D<float> RevealageTexture : register(t2);
Texture2D<float> DepthTexture : register(t3);
Texture2D<float2> VelocityTexture : register(t4);

struct PSOutput
{
    float4 Color : SV_TARGET0;
    float Depth : SV_TARGET1;
    float2 Velocity : SV_TARGET2;
};

PSOutput PS(VSOutput input)
{
    PSOutput output = (PSOutput)0.0f;

    uint ssaaMultiplier = CBComposition.SSAAMultiplier > 0 ? CBComposition.SSAAMultiplier : 1;
    int2 outputPixel = int2(input.Position.xy);
    int2 basePixel = outputPixel * int(ssaaMultiplier);
    float invSampleCount = 1.0f / float(ssaaMultiplier * ssaaMultiplier);

    float3 resolvedColor = 0.0f;
    float resolvedAlpha = 0.0f;
    float resolvedDepth = 0.0f;
    float2 resolvedVelocity = 0.0f;

    [loop]
    for (uint y = 0; y < ssaaMultiplier; y++)
    {
        [loop]
        for (uint x = 0; x < ssaaMultiplier; x++)
        {
            int2 samplePixel = basePixel + int2(x, y);

            float4 opaqueColor = OpaqueTexture.Load(int3(samplePixel, 0));
            float4 accumColor = AccumTexture.Load(int3(samplePixel, 0));
            float revealage = saturate(RevealageTexture.Load(int3(samplePixel, 0)));
            float depth = DepthTexture.Load(int3(samplePixel, 0));
            float2 velocity = VelocityTexture.Load(int3(samplePixel, 0));

            resolvedColor += opaqueColor.rgb * (1.0 - revealage) + accumColor.rgb;
            resolvedAlpha += max(opaqueColor.a, revealage);
            resolvedDepth = max(resolvedDepth, depth);
            resolvedVelocity += velocity;
        }
    }

    output.Color = float4(resolvedColor * invSampleCount, resolvedAlpha * invSampleCount);
    output.Depth = resolvedDepth;
    output.Velocity = resolvedVelocity * invSampleCount;
    return output;
}
