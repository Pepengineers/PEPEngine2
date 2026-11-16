#include "FullScreenVS.hlsl"
#include "CBufferStructures.hlsl"
ConstantBuffer<CameraCB> CBCamera : register(b0);
#define SKY_CB_REGISTER b1
#define SKY_TRANSMITTANCE_REGISTER t2
#define SKY_AERIAL_RADIANCE_REGISTER t3
#define SKY_AERIAL_TRANSMITTANCE_REGISTER t4
#include "SkyRendering.hlsli"
Texture2D<float4> InputHDR : register(t0);
Texture2D<float> InputDepth : register(t1);
SamplerState PointClamp : register(s1);
float4 PS(VSOutput input) : SV_TARGET
{
    float4 color = InputHDR.SampleLevel(PointClamp, input.TexCoord, 0);
    float depth = InputDepth.SampleLevel(PointClamp, input.TexCoord, 0);
    if (depth > 0 && CBSky.Mode.x == 2)
    {
        float4 world = mul(float4(input.TexCoord * float2(2, -2) + float2(-1, 1), depth, 1), CBCamera.InvViewProj);
        color.rgb = ApplySkyAerialPerspective(color.rgb, input.TexCoord,
            length(world.xyz / world.w - CBCamera.CameraLocation));
    }
    return color;
}
