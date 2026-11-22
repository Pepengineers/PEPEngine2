#ifndef GRASS_COMMON_HLSLI
#define GRASS_COMMON_HLSLI
struct GrassVertex
{
    float3 Position;
    float3 PreviousPosition;
    float3 Normal;
    float2 UV;
};
cbuffer GrassParameters : register(b0)
{
    float4x4 World;
    float4x4 PreviousWorld;
    float4x4 ViewProjection;
    float4 EyeMaxDistance;
    float3 PreviousEye; uint ResetHistory;
    float4 Shape;
    float3 Lod; uint InteractionSize;
    float2 PatchExtents; uint2 HiZSize;
    float4 WindArea;
    float4 InteractionArea;
    uint Count, Segments, Implementation, Seed;
    uint PointSource, MaterialIndex, GridResolution, Shadow;
};
#endif
