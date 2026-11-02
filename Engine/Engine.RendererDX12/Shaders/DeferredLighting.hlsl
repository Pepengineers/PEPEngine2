#include "CBufferStructures.hlsl"
#define LOCAL_REFLECTION_PROBES 1
ConstantBuffer<MainCB> CBMain : register(b0);
ConstantBuffer<CameraCB> CBCamera : register(b1);
ConstantBuffer<ShadowCB> CBShadow : register(b2);
Texture2D<float4> GAlbedo : register(t0);
Texture2D<float4> GNormal : register(t1);
Texture2D<float4> GF0 : register(t2);
Texture2D<float4> GEmissive : register(t3);
Texture2D<float> GDepth : register(t4);
Texture2D<float> ShadowAtlas : register(t5);
TextureCube<float4> IrradianceEnvironment : register(t6);
TextureCube<float4> PrefilteredEnvironment : register(t7);
Texture2D<float4> EnvironmentBRDF : register(t8);
TextureCube<float4> SourceEnvironment : register(t9);
Texture2DArray<float> LocalShadowMaps : register(t10);
StructuredBuffer<LocalLight> LocalLights : register(t11);
Texture2D<float> AmbientOcclusion : register(t14);
RWTexture2D<float4> SceneColor : register(u0);
SamplerState samLinearClamp : register(s3);
SamplerComparisonState samShadow : register(s6);
#define SKY_CB_REGISTER b3
#define SKY_TRANSMITTANCE_REGISTER t15
#include "SkyRendering.hlsli"
Texture2D<float4> VisibleSkyView : register(t16);
#define PBR_LIGHTING_ONLY
#include "PBRSurface.hlsli"
#define REFLECTION_PROBE_BINDINGS_DEFERRED 1
#include "ReflectionProbes.hlsli"
#include "PBRLighting.hlsli"

#ifndef DEFERRED_WAVE_OPS
#define DEFERRED_WAVE_OPS 0
#endif

cbuffer LightGrid : register(b4)
{
    uint4 GridSize;
    float4 GridDepth;
};
StructuredBuffer<uint> LightMasks : register(t17);

float3 WorldPosition(uint2 pixel, uint2 size, float depth)
{
    float2 ndc = (float2(pixel) + 0.5f) / size * float2(2, -2) + float2(-1, 1);
    float4 world = mul(float4(ndc, depth, 1), CBCamera.InvViewProj);
    return world.xyz / world.w;
}

[numthreads(16, 16, 1)]
void CS(uint3 dispatch : SV_DispatchThreadID)
{
    uint width, height;
    SceneColor.GetDimensions(width, height);
    uint2 size = uint2(width, height), pixel = dispatch.xy;
    if (any(pixel >= size)) return;
    float depth = GDepth[pixel];
    if (depth <= 0)
    {
        // A finite far depth also supports cameras with an infinite reverse-Z far plane.
        float3 ray = WorldPosition(pixel, size, 0.000001f) - CBCamera.CameraLocation;
        ray = SafeNormalize(ray);
        float3 sky = CBSky.Mode.x == 2 ? VisibleSkyView.SampleLevel(SkySampler,
            SkyViewUV(ray, CBSky.CameraPosition.xyz, CBSky.CenterRadius.w), 0).rgb :
            SourceEnvironment.SampleLevel(samLinearClamp, ray, 0).rgb;
        sky += SkySunDisk(ray, CBCamera.CameraLocation);
        SceneColor[pixel] = float4(sky, 1);
        return;
    }
    float3 position = WorldPosition(pixel, size, depth);
    float4 albedo = GAlbedo[pixel], normal = GNormal[pixel], f0 = GF0[pixel];
    PBRSurface s;
    s.Albedo = albedo.rgb; s.Metallic = albedo.a;
    s.Normal = SafeNormalize(normal.xyz); s.Roughness = normal.w;
    s.F0 = f0.rgb; s.Baked = f0.a > 0.5f;
    s.Emissive = GEmissive[pixel].rgb;
    float3 v = SafeNormalize(CBCamera.CameraLocation - position);
    float3 result = ShadeSunAndIBL(s, position, v, AmbientOcclusion[pixel]);
    if (s.Baked != 0)
    {
        SceneColor[pixel] = float4(result, 1);
        return;
    }
#ifdef DEFERRED_REFERENCE
    for (uint j = 0; j < CBMain.LocalLightCount; ++j)
        result += ShadeLocalLight(s, position, v, LocalLights[j]);
#else
    float viewDepth = max(-mul(float4(position, 1), CBCamera.View).z, GridDepth.w);
    uint slice = (uint)clamp(log2(viewDepth) * GridDepth.x + GridDepth.y, 0.0f, (float)(GridSize.z - 1));
    uint2 cell = min(pixel / (uint)GridDepth.z, GridSize.xy - 1);
    uint address = ((slice * GridSize.y + cell.y) * GridSize.x + cell.x) * GridSize.w;
    for (uint word = 0; word < GridSize.w; ++word)
    {
        uint mask = LightMasks[address + word];
#if DEFERRED_WAVE_OPS
        // Merge by source, so a light shared by several cells is visited once per wave.
        uint merged = WaveActiveBitOr(mask);
        while (merged != 0)
        {
            uint bit = firstbitlow(merged);
            merged &= merged - 1;
            uint index = WaveReadLaneFirst(word * 32 + bit);
            if ((mask & (1u << bit)) != 0)
                result += ShadeLocalLight(s, position, v, LocalLights[index]);
        }
#else
        while (mask != 0)
        {
            uint bit = firstbitlow(mask);
            mask &= mask - 1;
            result += ShadeLocalLight(s, position, v, LocalLights[word * 32 + bit]);
        }
#endif
    }
#endif
    SceneColor[pixel] = float4(result, 1);
}
