#include "CBufferStructures.hlsl"
#include "PBRMath.hlsli"
ConstantBuffer<CameraCB> Camera : register(b0);
Texture2D<float4> LitScene : register(t0);
Texture2D<float> Depth : register(t1);
Texture2D<float4> NormalRoughness : register(t2);
Texture2D<float4> F0Baked : register(t3);
Texture2D<float> AmbientVisibility : register(t4);
TextureCube<float4> PrefilteredEnvironment : register(t5);
Texture2D<float4> EnvironmentBRDF : register(t6);
Texture2D<float4> ReflectedRadiance : register(t7);
RWTexture2D<float4> Result : register(u0);
SamplerState samLinearClamp : register(s3);
#define PROBE_GLOBALS_REGISTER b1
#define PROBE_DATA_REGISTER t8
#define PROBE_CUBES_REGISTER t9
#include "ProbeIBL.hlsli"

[numthreads(8, 8, 1)]
void CS(uint2 pixel : SV_DispatchThreadID)
{
    uint width, height; Result.GetDimensions(width, height);
    if (any(pixel >= uint2(width, height))) return;
    float4 color = LitScene[pixel], material = F0Baked[pixel];
    float depth = Depth[pixel];
    if (depth <= 0 || material.a > 0.5) { Result[pixel] = color; return; }
    float2 ndc = (float2(pixel) + 0.5) / uint2(width, height) * float2(2, -2) + float2(-1, 1);
    float4 world = mul(float4(ndc, depth, 1), Camera.InvViewProj);
    float3 view = Camera.CameraLocation - world.xyz / world.w;
    view *= rsqrt(max(dot(view, view), 1e-12));
    float4 normalRoughness = NormalRoughness[pixel];
    float3 normal = normalRoughness.xyz * rsqrt(max(dot(normalRoughness.xyz, normalRoughness.xyz), 1e-12));
    float nDotV = saturate(dot(normal, view));
    float2 brdf = EnvironmentBRDF.SampleLevel(samLinearClamp, float2(nDotV, normalRoughness.a), 0).rg;
    float3 weight = SpecularIBLWeight(material.rgb, brdf, nDotV, normalRoughness.a, AmbientVisibility[pixel]);
    float3 oldRadiance = SpecularEnvironmentRadiance(world.xyz / world.w, reflect(-view, normal), normalRoughness.a);
    Result[pixel] = float4(max(0, color.rgb + (ReflectedRadiance[pixel].rgb - oldRadiance) * weight), color.a);
}
