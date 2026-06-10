#include "CBufferStructures.hlsl"
#include "FullScreenVS.hlsl"

SamplerState samPointWrap : register(s0);
SamplerState samPointClamp : register(s1);
SamplerState samLinearWrap : register(s2);
SamplerState samLinearClamp : register(s3);
SamplerState samAnisotropicWrap : register(s4);
SamplerState samAnisotropicClamp : register(s5);

Texture2D<float4> OpaqueTexture : register(t0);
Texture2D<float4> AccumTexture : register(t1);
Texture2D<float> RevealageTexture : register(t2);

float4 PS(VSOutput input) : SV_TARGET
{
    float2 uv = input.TexCoord;
    
    float4 opaqueColor = OpaqueTexture.Sample(samPointClamp, uv);
    float4 accumColor = AccumTexture.Sample(samPointClamp, uv);
    float revealage = RevealageTexture.Sample(samPointClamp, uv);
  
    
    float3 finalColor = opaqueColor.rgb * (1.0 - revealage) + accumColor.rgb;
    float finalAlpha = max(opaqueColor.a, revealage);
    
    return float4(finalColor, finalAlpha);
}