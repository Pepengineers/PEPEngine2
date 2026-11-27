#include "FullScreenVS.hlsl"

SamplerState samPointClamp : register(s1);

Texture2D<float4> OpaqueTexture : register(t0);
Texture2D<float4> AccumTexture : register(t1);
Texture2D<float> RevealageTexture : register(t2);

struct PS_OUTPUT
{
    float4 Color : SV_TARGET0;
    float TransparencyMask : SV_TARGET1;
};

PS_OUTPUT PS(VSOutput input)
{
    float2 uv = input.TexCoord;
    
    float3 opaqueColor = OpaqueTexture.Sample(samPointClamp, uv).rgb;
    float4 accumColor = AccumTexture.Sample(samPointClamp, uv);
    float revealage = saturate(RevealageTexture.Sample(samPointClamp, uv));
    
    float3 transparentColor = accumColor.rgb / max(accumColor.a, 1e-5f);
    float3 finalColor = lerp(transparentColor, opaqueColor, revealage);
    
    PS_OUTPUT output;
    output.Color = float4(finalColor, 1.0f);
    // WBOIT revealage is the fraction of the background that remains visible.
    output.TransparencyMask = 1.0f - revealage;
    return output;
}
