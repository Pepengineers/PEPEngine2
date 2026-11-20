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
    
    float3 opaqueColor = OpaqueTexture.Sample(samPointClamp, uv).rgb;
    float4 accumColor = AccumTexture.Sample(samPointClamp, uv);
    float revealage = saturate(RevealageTexture.Sample(samPointClamp, uv));
    
    float3 finalColor = opaqueColor * (1.0 - revealage) + accumColor.rgb;
    
    return float4(finalColor, 1.0f);
}