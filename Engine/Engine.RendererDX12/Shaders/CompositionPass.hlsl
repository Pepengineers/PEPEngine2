#include "PBRMath.hlsli"
#include "FullScreenVS.hlsl"

SamplerState samPointClamp : register(s1);

Texture2D<float4> OpaqueTexture : register(t0);
Texture2D<float4> AccumTexture : register(t1);
Texture2D<float> RevealageTexture : register(t2);

float4 PS(VSOutput input) : SV_TARGET
{
    float2 uv = input.TexCoord;
    
    float3 opaqueColor = OpaqueTexture.Sample(samPointClamp, uv).rgb;
    float4 accumColor = AccumTexture.Sample(samPointClamp, uv);
    float revealage = saturate(RevealageTexture.Sample(samPointClamp, uv));
    
    float3 transparentColor = accumColor.rgb / max(accumColor.a, 1e-5f);
    // Deferred lighting has already tone-mapped the opaque background.
    float3 finalColor = lerp(ToneMapToSRGB(transparentColor), opaqueColor, revealage);
    
    return float4(finalColor, 1.0f);
}