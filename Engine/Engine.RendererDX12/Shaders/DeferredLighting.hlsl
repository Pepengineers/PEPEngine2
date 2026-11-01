#include "CBufferStructures.hlsl"
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
RWTexture2D<float4> SceneColor : register(u0);
SamplerState samLinearClamp : register(s3);
SamplerComparisonState samShadow : register(s6);
#define PBR_LIGHTING_ONLY
#include "PBRSurface.hlsli"
#include "PBRLighting.hlsli"

groupshared uint MinDepth, MaxDepth, LightCount;
groupshared uint LightIndices[512];

float3 WorldPosition(uint2 pixel, uint2 size, float depth)
{
    float2 ndc = (float2(pixel) + 0.5f) / size * float2(2, -2) + float2(-1, 1);
    float4 world = mul(float4(ndc, depth, 1), CBCamera.InvViewProj);
    return world.xyz / world.w;
}
bool IntersectsPlane(float4 plane, LocalLight light)
{
    return dot(float4(light.Position, 1), plane) >= -light.Range * length(plane.xyz);
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
    if (lane == 0) { MinDepth = 0x7f7fffffu; MaxDepth = 0; LightCount = 0; }
    GroupMemoryBarrierWithGroupSync();
    if (depth > 0)
    {
        uint z = asuint(max(0.0f, -mul(float4(position, 1), CBCamera.View).z));
        InterlockedMin(MinDepth, z);
        InterlockedMax(MaxDepth, z);
    }
    GroupMemoryBarrierWithGroupSync();
    float2 lo = float2(group.xy * 16) / size * 2 - 1;
    float2 hi = float2(min(group.xy * 16 + 16, size)) / size * 2 - 1;
    float4x4 m = transpose(CBCamera.ViewProj);
    for (uint i = lane; i < CBMain.LocalLightCount && MaxDepth != 0; i += 256)
    {
        LocalLight light = LocalLights[i];
        float z = -mul(float4(light.Position, 1), CBCamera.View).z;
        if (z + light.Range < asfloat(MinDepth) || z - light.Range > asfloat(MaxDepth)) continue;
        if (!IntersectsPlane(m[0] - lo.x * m[3], light) || !IntersectsPlane(hi.x * m[3] - m[0], light) ||
            !IntersectsPlane(m[1] + hi.y * m[3], light) || !IntersectsPlane(-lo.y * m[3] - m[1], light)) continue;
        uint slot;
        InterlockedAdd(LightCount, 1, slot);
        if (slot < 512) LightIndices[slot] = i;
    }
    GroupMemoryBarrierWithGroupSync();
    if (!valid) return;
    if (depth <= 0)
    {
        // A finite far depth also supports cameras with an infinite reverse-Z far plane.
        float3 ray = WorldPosition(pixel, size, 0.000001f) - CBCamera.CameraLocation;
        SceneColor[pixel] = float4(SourceEnvironment.SampleLevel(samLinearClamp, SafeNormalize(ray), 0).rgb, 1);
        return;
    }
    float4 albedo = GAlbedo[pixel], normal = GNormal[pixel], f0 = GF0[pixel];
    PBRSurface s;
    s.Albedo = albedo.rgb; s.Metallic = albedo.a;
    s.Normal = SafeNormalize(normal.xyz); s.Roughness = normal.w;
    s.F0 = f0.rgb; s.Baked = f0.a > 0.5f;
    s.Emissive = GEmissive[pixel].rgb;
    float3 v = SafeNormalize(CBCamera.CameraLocation - position);
    float3 result = ShadeSunAndIBL(s, position, v);
    // Overflow preserves correctness for unusually dense groups of lights.
    uint count = LightCount > 512 ? CBMain.LocalLightCount : LightCount;
    for (uint j = 0; j < count; ++j)
        result += ShadeLocalLight(s, position, v, LocalLights[LightCount > 512 ? j : LightIndices[j]]);
    SceneColor[pixel] = float4(result, 1);
}
