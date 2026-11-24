#include "CBufferStructures.hlsli"
#include "PBRMath.hlsli"
#include "FullScreenVS.hlsl"

ConstantBuffer<CameraCB> Camera : register(b0);
TextureCube<float4> SkyBoxMap : register(t0);
Texture2D<float> Depth : register(t1);
SamplerState samLinearClamp : register(s3);

struct PS_OUTPUT
{
    float4 Color : SV_TARGET0;
    float2 Velocity : SV_TARGET1;
};

PS_OUTPUT PS(VSOutput input)
{
    uint2 pixel = uint2(input.Position.xy);
    if (Depth[pixel] > 0) discard;

    uint width, height;
    Depth.GetDimensions(width, height);
    float2 ndc = (float2(pixel) + 0.5f) / float2(width, height) * float2(2, -2) + float2(-1, 1);
    float4 world = mul(float4(ndc, 0, 1), Camera.InvViewProj);
    // Bistro's sky reprojection uses a direction at infinity, excluding camera translation.
    float4 ray = float4(world.xyz - Camera.CameraLocation * world.w, 0);
    float4 current = mul(ray, Camera.ViewProjNoJitter);
    float4 previous = mul(ray, Camera.PrevViewProjNoJitter);
    float2 velocity = current.w > 1e-5f && previous.w > 1e-5f
        ? previous.xy / previous.w - current.xy / current.w : 0;

    PS_OUTPUT output;
    output.Color = float4(ToneMapToSRGB(SkyBoxMap.SampleLevel(samLinearClamp, SafeNormalize(ray.xyz), 0).rgb), 1);
    output.Velocity = all(isfinite(velocity)) ? velocity : 0;
    return output;
}
