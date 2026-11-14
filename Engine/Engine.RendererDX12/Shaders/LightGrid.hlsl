#include "CBufferStructures.hlsl"

cbuffer BuildParameters : register(b0)
{
    uint Width, Height, LightCount, BuildGroups;
    float4 ProjectionScaleOffset;
    float FarPlane;
};
ConstantBuffer<CameraCB> CBCamera : register(b1);
cbuffer LightGrid : register(b2)
{
    uint4 GridSize;
    float4 GridDepth;
};
StructuredBuffer<LocalLight> LocalLights : register(t0);
RWStructuredBuffer<uint> LightMasks : register(u0);

float2 ProjectInterval(float center, float radial, float front, float back, float scale, float offset)
{
    float2 low = (center - radial) / float2(front, back);
    float2 high = (center + radial) / float2(front, back);
    return float2(min(low.x, low.y), max(high.x, high.y)) * scale - offset;
}

uint DepthSlice(float z)
{
    return (uint)clamp(log2(max(z, GridDepth.w)) * GridDepth.x + GridDepth.y, 0.0f, (float)(GridSize.z - 1));
}

[numthreads(64, 1, 1)]
void BuildCS(uint3 group : SV_GroupID, uint lane : SV_GroupIndex)
{
    uint groupIndex = group.x + group.y * 65535u;
    if (groupIndex >= BuildGroups) return;
    if (LightCount == 0)
    {
        if (lane == 0) LightMasks[0] = 0;
        return;
    }
    uint cellCount = GridSize.x * GridSize.y;
    uint groupsPerSlice = (cellCount + 63) / 64;
    uint cellIndex = (groupIndex % groupsPerSlice) * 64 + lane;
    if (cellIndex >= cellCount) return;
    uint sliceWord = groupIndex / groupsPerSlice;
    uint slice = sliceWord % GridSize.z;
    uint word = sliceWord / GridSize.z;
    uint2 cell = uint2(cellIndex % GridSize.x, cellIndex / GridSize.x);
    uint firstLight = word * 32;
#if LIGHT_GRID_WAVE_OPS
    firstLight = WaveReadLaneFirst(firstLight);
    uint2 waveFirst = uint2(WaveActiveMin(cell.x), WaveActiveMin(cell.y));
    uint2 waveLast = uint2(WaveActiveMax(cell.x), WaveActiveMax(cell.y));
#endif
    bool infinite = FarPlane == 0 || isinf(FarPlane);
    float sliceNear = slice == 0 ? GridDepth.w : GridDepth.w * exp2((float)slice / GridDepth.x);
    float sliceFar = slice + 1 == GridSize.z ? (infinite ? 3.402823466e+38f : FarPlane) :
        GridDepth.w * exp2((float)(slice + 1) / GridDepth.x);
    uint mask = 0;
    [loop] for (uint bit = 0; bit < min(32u, LightCount - firstLight); ++bit)
    {
        uint lightIndex = firstLight + bit;
#if LIGHT_GRID_WAVE_OPS
        lightIndex = WaveReadLaneFirst(lightIndex);
#endif
        LocalLight light = LocalLights[lightIndex];
        float3 center = mul(float4(light.Position, 1), CBCamera.View).xyz;
        float z = -center.z, radius = light.Range;
        if (!all(isfinite(center)) || !isfinite(radius) || radius <= 0) continue;
        float margin = max(abs(z), radius) * 1e-5f;
        float lo = max(GridDepth.w, z - radius - margin);
        float hi = min(infinite ? 3.402823466e+38f : FarPlane, z + radius + margin);
        if (hi < lo || slice < DepthSlice(lo) || slice > DepthSlice(hi)) continue;
        float front = max(lo, sliceNear), back = min(hi, sliceFar);
        if (back < front) continue;
        float dz = abs(clamp(z, front, back) - z) / radius;
        float radial = radius * sqrt(max(0.0f, 1 - dz * dz)) + margin;
        float2 x = ProjectInterval(center.x, radial, front, back, ProjectionScaleOffset.x, ProjectionScaleOffset.z);
        float2 y = ProjectInterval(center.y, radial, front, back, ProjectionScaleOffset.y, ProjectionScaleOffset.w);
        if (x.y < -1 || x.x > 1 || y.y < -1 || y.x > 1) continue;
        uint2 first = (uint2)clamp(floor((float2(x.x + 1, 1 - y.y) * 0.5f * float2(Width, Height) - 1e-3f) / GridDepth.z),
            0.0f, (float2)(GridSize.xy - 1));
        uint2 last = (uint2)clamp(floor((float2(x.y + 1, 1 - y.x) * 0.5f * float2(Width, Height) + 1e-3f) / GridDepth.z),
            0.0f, (float2)(GridSize.xy - 1));
#if LIGHT_GRID_WAVE_OPS
        if (any(last < waveFirst) || any(first > waveLast)) continue;
#endif
        if (all(cell >= first) && all(cell <= last)) mask |= 1u << bit;
    }
    LightMasks[(slice * cellCount + cellIndex) * GridSize.w + word] = mask;
}
