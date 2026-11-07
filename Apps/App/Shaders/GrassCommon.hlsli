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
    float4 PreviousEye;
    float4 Shape;
    float4 Lod;
    float4 Patch;
    float4 WindArea;
    float4 WindForce;
    float4 WindGust;
    float4 InteractionArea;
    uint Count, Segments, Implementation, Seed;
    uint PointSource, MaterialIndex, WindMode, Shadow;
    uint GridResolution, ResetHistory, Padding0, Padding1;
};
#endif
