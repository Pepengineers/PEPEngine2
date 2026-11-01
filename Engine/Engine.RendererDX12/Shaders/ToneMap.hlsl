#include "FullScreenVS.hlsl"
#include "PBRMath.hlsli"
Texture2D<float4> Scene : register(t0);
SamplerState LinearClamp : register(s3);

float4 PS(VSOutput input) : SV_TARGET
{
    float3 hdr = Scene.SampleLevel(LinearClamp, input.TexCoord, 0).rgb;
    return float4(ToneMapToSRGB(hdr.r), ToneMapToSRGB(hdr.g), ToneMapToSRGB(hdr.b), 1);
}
