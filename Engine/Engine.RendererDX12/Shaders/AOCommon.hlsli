#ifndef AO_COMMON_HLSLI
#define AO_COMMON_HLSLI
#include "CBufferStructures.hlsl"
cbuffer AOParameters : register(b0)
{
    float4x4 Projection;
    float4x4 InverseProjection;
    uint2 Size;
    uint2 WorkingSize;
    float Radius;
    float Intensity;
    float Bias;
    uint Samples;
    uint Horizontal;
    uint RestorePackedScale;
    uint2 Padding;
};
ConstantBuffer<CameraCB> CBCamera : register(b1);
Texture2D<float> Depth : register(t0);
Texture2D<float4> Normals : register(t1);
Texture2D<float> SourceAO : register(t2);
RWTexture2D<float> DestinationAO : register(u0);
RWTexture2D<float> PreparedDepth : register(u1);
RWTexture2D<float4> PreparedNormals : register(u2);

int2 ClampPixel(int2 pixel) { return clamp(pixel, 0, int2(Size) - 1); }
float3 ViewPosition(int2 pixel)
{
    pixel = ClampPixel(pixel);
    float2 ndc = (float2(pixel) + 0.5f) / Size * float2(2, -2) + float2(-1, 1);
    float4 position = mul(float4(ndc, max(Depth[pixel], 1e-8f), 1), InverseProjection);
    position.xyz /= position.w;
    return float3(position.xy, -position.z);
}
float3 ViewNormal(int2 pixel) { return Normals[ClampPixel(pixel)].xyz; }
float2 ProjectView(float3 positiveZPosition)
{
    float4 clip = mul(float4(positiveZPosition.xy, -positiveZPosition.z, 1), Projection);
    return clip.xy / clip.w * float2(0.5f, -0.5f) + 0.5f;
}
float Hash(uint2 pixel)
{
    uint v = pixel.x * 1664525u + pixel.y * 1013904223u;
    v ^= v >> 16;
    v *= 2246822519u;
    return (v & 0x00ffffffu) / 16777216.0f;
}
float ApplyIntensity(float visibility) { return saturate(1 - (1 - saturate(visibility)) * Intensity); }
#endif
