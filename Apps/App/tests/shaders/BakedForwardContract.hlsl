// Instrument the real ShadePBR caller without depending on GPU execution.
#include "CBufferStructures.hlsl"
#define PBR_LIGHTING_ONLY
#include "PBRSurface.hlsli"
#define PBR_LIGHTING_HLSLI

ConstantBuffer<MainCB> CBMain : register(b0);
ConstantBuffer<CameraCB> CBCamera : register(b1);
StructuredBuffer<LocalLight> LocalLights : register(t0);
RWStructuredBuffer<uint> Calls : register(u0);
RWStructuredBuffer<float3> Result : register(u1);

PBRSurface SamplePBRSurface(Material material, float3 albedo, float3 position, float3 normal, float2 uv)
{
    InterlockedAdd(Calls[0], 1);
    PBRSurface surface = (PBRSurface)0;
    surface.Albedo = albedo;
    surface.Baked = TEST_BAKED;
    return surface;
}

float3 ShadeSunAndIBL(PBRSurface surface, float3 position, float3 view)
{
    InterlockedAdd(Calls[1], 1);
    return surface.Albedo;
}

float3 ShadeLocalLight(PBRSurface surface, float3 position, float3 view, LocalLight light)
{
    InterlockedAdd(Calls[2], 1);
    return 0;
}

#include "PBR.hlsli"

[numthreads(1, 1, 1)]
void CS()
{
    Result[0] = ShadePBR((Material)0, float3(0.25f, 0.5f, 0.75f), 0, float3(0, 1, 0), 0);
}
