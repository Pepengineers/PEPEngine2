#include "FullScreenVS.hlsl"
#include "PBRMath.hlsli"
Texture2D<float4> HDRColor : register(t0);

float4 PS(VSOutput input) : SV_TARGET
{
    float3 color = HDRColor.Load(int3(input.Position.xy, 0)).rgb;
    return float4(ToneMapToSRGB(color.r), ToneMapToSRGB(color.g), ToneMapToSRGB(color.b), 1.0f);
}
