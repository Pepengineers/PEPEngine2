#include "FullScreenVS.hlsl"
#include "PBRMath.hlsli"
Texture2D<float4> SceneHDR : register(t0);

float4 PS(VSOutput input) : SV_TARGET
{
    float4 color = SceneHDR.Load(int3(input.Position.xy, 0));
    return float4(ToneMapToSRGB(color.r), ToneMapToSRGB(color.g), ToneMapToSRGB(color.b), color.a);
}
