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

#define CLUSTER_SLICES 32
#define CLUSTER_LIGHT_CAPACITY 512
#define CLUSTER_WORDS (CLUSTER_LIGHT_CAPACITY / 32)
groupshared uint MinDepth, MaxDepth, LightCount, OccupiedSlices;
groupshared uint LightIndices[CLUSTER_LIGHT_CAPACITY];
groupshared uint ClusterMasks[CLUSTER_SLICES * CLUSTER_WORDS];
groupshared float4 TilePlanes[4];

float3 WorldPosition(uint2 pixel, uint2 size, float depth)
{
    float2 ndc = (float2(pixel) + 0.5f) / size * float2(2, -2) + float2(-1, 1);
    float4 world = mul(float4(ndc, depth, 1), CBCamera.InvViewProj);
    return world.xyz / world.w;
}
uint DepthSlice(float z, float logMin, float logScale)
{
    return (uint)clamp((log2(max(z, max(CBCamera.NearPlane, 1e-4f))) - logMin) * logScale,
        0.0f, (float)(CLUSTER_SLICES - 1));
}

[numthreads(16, 16, 1)]
void CS(uint3 dispatch : SV_DispatchThreadID, uint3 group : SV_GroupID, uint lane : SV_GroupIndex)
{
    uint width, height;
    SceneColor.GetDimensions(width, height);
    uint2 size = uint2(width, height), pixel = dispatch.xy;
    bool valid = all(pixel < size);
    float depth = valid ? GDepth[pixel] : 0;
    float3 position = depth > 0 ? WorldPosition(pixel, size, depth) : 0;
    float viewDepth = max(max(CBCamera.NearPlane, 1e-4f), -mul(float4(position, 1), CBCamera.View).z);
#ifndef DEFERRED_REFERENCE
    if (lane == 0)
    {
        MinDepth = 0x7f7fffffu; MaxDepth = 0; LightCount = 0; OccupiedSlices = 0;
        float2 lo = float2(group.xy * 16) / size * 2 - 1;
        float2 hi = float2(min(group.xy * 16 + 16, size)) / size * 2 - 1;
        float4x4 m = transpose(CBCamera.ViewProj);
        TilePlanes[0] = m[0] - lo.x * m[3];
        TilePlanes[1] = hi.x * m[3] - m[0];
        TilePlanes[2] = m[1] + hi.y * m[3];
        TilePlanes[3] = -lo.y * m[3] - m[1];
        [unroll] for (uint p = 0; p < 4; ++p) TilePlanes[p] /= length(TilePlanes[p].xyz);
    }
    for (uint clear = lane; clear < CLUSTER_SLICES * CLUSTER_WORDS; clear += 256) ClusterMasks[clear] = 0;
    GroupMemoryBarrierWithGroupSync();
#if DEFERRED_WAVE_OPS
    uint waveMin = WaveActiveMin(depth > 0 ? asuint(viewDepth) : 0x7f7fffffu);
    uint waveMax = WaveActiveMax(depth > 0 ? asuint(viewDepth) : 0u);
    if (WaveIsFirstLane())
    {
        InterlockedMin(MinDepth, waveMin);
        InterlockedMax(MaxDepth, waveMax);
    }
#else
    if (depth > 0)
    {
        InterlockedMin(MinDepth, asuint(viewDepth));
        InterlockedMax(MaxDepth, asuint(viewDepth));
    }
#endif
    GroupMemoryBarrierWithGroupSync();
    float logMin = log2(asfloat(MinDepth));
    float logScale = CLUSTER_SLICES / max(log2(max(asfloat(MaxDepth), asfloat(MinDepth))) - logMin, 1e-4f);
    uint cluster = DepthSlice(viewDepth, logMin, logScale);
#if DEFERRED_WAVE_OPS
    uint occupied = WaveActiveBitOr(depth > 0 ? 1u << cluster : 0u);
    if (WaveIsFirstLane()) InterlockedOr(OccupiedSlices, occupied);
#else
    if (depth > 0) InterlockedOr(OccupiedSlices, 1u << cluster);
#endif
    GroupMemoryBarrierWithGroupSync();
    for (uint base = 0; base < CBMain.LocalLightCount && MaxDepth != 0; base += 256)
    {
        uint i = base + lane, sliceMask = 0;
        if (i < CBMain.LocalLightCount)
        {
            // Culling fetches only the sphere, not the light's six shadow matrices.
            float3 center = LocalLights[i].Position;
            float radius = LocalLights[i].Range;
            float z = -mul(float4(center, 1), CBCamera.View).z;
            bool intersects = z + radius >= asfloat(MinDepth) && z - radius <= asfloat(MaxDepth);
            [unroll] for (uint p = 0; p < 4; ++p)
                intersects = intersects && dot(float4(center, 1), TilePlanes[p]) >= -radius;
            if (intersects)
            {
                // Expand endpoints for reconstruction/log rounding at slice boundaries.
                uint first = DepthSlice(z - radius - max(abs(z), radius) * 1e-5f, logMin, logScale);
                uint last = DepthSlice(z + radius + max(abs(z), radius) * 1e-5f, logMin, logScale);
                sliceMask = (0xffffffffu << first) & (0xffffffffu >> (31u - last)) & OccupiedSlices;
            }
        }
        uint slot = 0;
#if DEFERRED_WAVE_OPS
        uint prefix = WavePrefixCountBits(sliceMask != 0);
        uint accepted = WaveActiveCountBits(sliceMask != 0);
        if (WaveIsFirstLane() && accepted != 0) InterlockedAdd(LightCount, accepted, slot);
        slot = WaveReadLaneFirst(slot) + prefix;
#else
        if (sliceMask != 0) InterlockedAdd(LightCount, 1, slot);
#endif
        if (sliceMask != 0 && slot < CLUSTER_LIGHT_CAPACITY)
        {
            LightIndices[slot] = i;
            while (sliceMask != 0)
            {
                uint slice = firstbitlow(sliceMask);
                sliceMask &= sliceMask - 1;
                InterlockedOr(ClusterMasks[slice * CLUSTER_WORDS + slot / 32], 1u << (slot & 31));
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();
#endif
    if (!valid) return;
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
#ifndef DEFERRED_REFERENCE
    if (LightCount <= CLUSTER_LIGHT_CAPACITY)
    {
#if DEFERRED_WAVE_OPS
        bool coherentCluster = WaveActiveAllEqual(cluster);
#endif
        for (uint word = 0; word < (LightCount + 31) / 32; ++word)
        {
            uint mask = ClusterMasks[cluster * CLUSTER_WORDS + word];
#if DEFERRED_WAVE_OPS
            if (coherentCluster)
            {
                mask = WaveReadLaneFirst(mask);
                while (mask != 0)
                {
                    uint bit = firstbitlow(mask);
                    mask &= mask - 1;
                    uint index = WaveReadLaneFirst(LightIndices[word * 32 + bit]);
                    result += ShadeLocalLight(s, position, v, LocalLights[index]);
                }
                continue;
            }
            uint waveMask = WaveActiveBitOr(mask);
            while (waveMask != 0)
            {
                uint bit = firstbitlow(waveMask);
                waveMask &= waveMask - 1;
                // A uniform list address lets the driver scalarize the light data loads.
                uint index = WaveReadLaneFirst(LightIndices[word * 32 + bit]);
                if ((mask & (1u << bit)) != 0)
                    result += ShadeLocalLight(s, position, v, LocalLights[index]);
            }
#else
            while (mask != 0)
            {
                uint bit = firstbitlow(mask);
                mask &= mask - 1;
                result += ShadeLocalLight(s, position, v, LocalLights[LightIndices[word * 32 + bit]]);
            }
#endif
        }
    }
    else
#endif
    {
        // shortcut: over 512 tile candidates use all lights; grow/stream lists if profiling warrants it.
        for (uint j = 0; j < CBMain.LocalLightCount; ++j)
            result += ShadeLocalLight(s, position, v, LocalLights[j]);
    }
    SceneColor[pixel] = float4(result, 1);
}
