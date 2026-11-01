#include "FullScreenVS.hlsl"
#include "PBRMath.hlsli"
Texture2D<float4> SceneColor : register(t0);
SamplerState samLinearClamp : register(s3);
float4 PS(VSOutput input) : SV_TARGET
{
    float4 color = SceneColor.SampleLevel(samLinearClamp, input.TexCoord, 0);
    return float4(ToneMapToSRGB(color.r), ToneMapToSRGB(color.g), ToneMapToSRGB(color.b), color.a);
}
