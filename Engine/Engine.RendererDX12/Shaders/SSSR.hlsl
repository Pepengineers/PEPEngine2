#include "CBufferStructures.hlsl"
#include "PBRMath.hlsli"
ConstantBuffer<CameraCB> CBCamera : register(b0);
cbuffer SSRConstants : register(b1)
{
    float2 JitterDeltaUV;
    float RoughnessCutoff;
    float Padding;
};
Texture2D<float4> Scene : register(t0);
Texture2D<float> Depth : register(t1);
Texture2D<float4> NormalRoughness : register(t2);
Texture2D<float4> F0Baked : register(t3);
Texture2D<float2> Motion : register(t4);
TextureCube<float4> Environment : register(t5);
Texture2D<float4> DFG : register(t6);
Texture2D<float4> Radiance : register(t7);
RWTexture2D<float> RoughnessOut : register(u0);
RWTexture2D<float2> MotionOut : register(u1);
RWTexture2D<float4> ColorOut : register(u2);
Texture2D<float4> Fallback : register(t8);
RWTexture2D<float4> FallbackOut : register(u3);
SamplerState LinearClamp : register(s3);
#define samLinearClamp LinearClamp
#include "ReflectionProbes.hlsli"
#undef samLinearClamp

[numthreads(8, 8, 1)]
void Prepare(uint3 id : SV_DispatchThreadID)
{
    uint width, height;
    RoughnessOut.GetDimensions(width, height);
    if (any(id.xy >= uint2(width, height))) return;
    float4 normal = NormalRoughness[id.xy];
    float roughness = normal.w;
    float2 ndc = (float2(id.xy) + 0.5f) / float2(width, height) * float2(2, -2) + float2(-1, 1);
    float4 world = mul(float4(ndc, Depth[id.xy], 1), CBCamera.InvViewProj);
    float3 position = world.xyz / world.w;
    float3 view = normalize(CBCamera.CameraLocation - position);
    float3 direction = reflect(-view, normalize(normal.xyz));
    float3 fallback = 0;
    if (Depth[id.xy] > 0 && F0Baked[id.xy].a < 0.5f)
    {
        fallback = Environment.SampleLevel(LinearClamp, direction, roughness * 7).rgb;
        fallback = SampleReflectionRadiance(position, direction, roughness, fallback);
    }
    FallbackOut[id.xy] = float4(fallback, 1);
    // Supply alpha roughness explicitly: SDK 1.1.4 forwards this flag to the denoiser too.
    RoughnessOut[id.xy] = Depth[id.xy] > 0 && F0Baked[id.xy].a < 0.5f ? roughness * roughness : 1;
    // SDK samples history at uv + motion; raster jitter is excluded from the shared motion buffer.
    MotionOut[id.xy] = Motion[id.xy] * float2(0.5f, -0.5f) + JitterDeltaUV;
}

[numthreads(8, 8, 1)]
void Resolve(uint3 id : SV_DispatchThreadID)
{
    uint width, height;
    ColorOut.GetDimensions(width, height);
    if (any(id.xy >= uint2(width, height))) return;
    float4 scene = Scene[id.xy];
    float4 normal = NormalRoughness[id.xy];
    float4 f0 = F0Baked[id.xy];
    float depth = Depth[id.xy];
    if (depth > 0 && f0.a < 0.5f && normal.w < RoughnessCutoff)
    {
        float2 ndc = (float2(id.xy) + 0.5f) / float2(width, height) * float2(2, -2) + float2(-1, 1);
        float4 world = mul(float4(ndc, depth, 1), CBCamera.InvViewProj);
        float3 v = normalize(CBCamera.CameraLocation - world.xyz / world.w);
        float3 n = normalize(normal.xyz);
        float2 brdf = DFG.SampleLevel(LinearClamp, float2(saturate(dot(n, v)), normal.w), 0).rg;
        float3 fallback = Fallback[id.xy].rgb;
        // The SDK already blends hits with environment fallback. Denoiser alpha is an internal payload.
        scene.rgb = max(0, scene.rgb + (Radiance[id.xy].rgb - fallback) * (f0.rgb * brdf.x + brdf.y));
    }
    ColorOut[id.xy] = scene;
}
