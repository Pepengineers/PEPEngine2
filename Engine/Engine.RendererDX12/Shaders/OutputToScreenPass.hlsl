#include "FullScreenVS.hlsl"

Texture2D<float4> SceneColor : register(t0);

float3 ToneMapToLDR(float3 color)
{
    color = max(color, 0.0f);
    return color / (1.0f + color);
}

float4 PS(VSOutput input) : SV_TARGET
{
    float3 color = SceneColor.Load(int3(int2(input.Position.xy), 0)).rgb;
    return float4(ToneMapToLDR(color), 1.0f);
}
