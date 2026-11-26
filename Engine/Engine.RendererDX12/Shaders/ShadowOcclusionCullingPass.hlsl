#include "CBufferStructures.hlsli"

cbuffer Parameters : register(b0)
{
    uint DrawOffset;
    uint ShadowMapResolution;
    uint TargetSize;
    uint FirstReduction;
    float4 MinOccluderSizeTexels;
};
StructuredBuffer<ShadowDrawArgs> Candidates : register(t0);
StructuredBuffer<uint> CandidateCount : register(t1);
StructuredBuffer<InstanceData> Instances : register(t2);
StructuredBuffer<Material> Materials : register(t3);
StructuredBuffer<ShadowView> Views : register(t4);
Texture2DArray<float> Depth : register(t5);
RWStructuredBuffer<ShadowDrawArgs> Occluders : register(u0);
RWStructuredBuffer<uint> Counts : register(u1);
RWStructuredBuffer<ShadowDrawArgs> Visible : register(u2);
RWTexture2DArray<float> ReducedDepth : register(u3);

bool IsOccluder(ShadowDrawArgs command)
{
    InstanceData instance = Instances[command.InstanceID];
    if (Materials[instance.MaterialIndex].RenderLayer != RENDER_LAYER_OPAQUE) return false;
    ShadowView view = Views[command.ShadowViewIndex];
    float3 projectionX = float3(view.ViewProj._11, view.ViewProj._21, view.ViewProj._31);
    float3 projectionY = float3(view.ViewProj._12, view.ViewProj._22, view.ViewProj._32);
    float2 size = float2(dot(abs(projectionX), instance.BoundingBoxExtents),
                        dot(abs(projectionY), instance.BoundingBoxExtents)) * ShadowMapResolution;
    return min(size.x, size.y) >= MinOccluderSizeTexels[view.Layer % 4];
}

[numthreads(1, 1, 1)]
void ClearCountsCS(uint3 id : SV_DispatchThreadID)
{
    Counts[0] = 0;
    Counts[1] = 0;
}

[numthreads(64, 1, 1)]
void SelectOccludersCS(uint3 id : SV_DispatchThreadID)
{
    uint index = DrawOffset + id.x;
    if (index >= CandidateCount[0]) return;
    ShadowDrawArgs command = Candidates[index];
    if (!IsOccluder(command)) return;
    uint offset;
    InterlockedAdd(Counts[0], 1, offset);
    Occluders[offset] = command;
}

// Shadow depth uses LESS_EQUAL with clear=1. Max reduction preserves every hole:
// a region can hide a candidate only when its furthest depth is still in front.
// The first level covers 2x2 shadow texels; power-of-two padding stays at clear depth.
[numthreads(8, 8, 1)]
void ReduceDepthCS(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= TargetSize)) return;
    uint2 source = id.xy * 2;
    float farthest = 0;
    for (uint y = 0; y < 2; ++y)
    for (uint x = 0; x < 2; ++x)
    {
        uint2 pixel = source + uint2(x, y);
        float depth = FirstReduction && any(pixel >= ShadowMapResolution) ? 1.0f : Depth.Load(int4(pixel, id.z, 0));
        farthest = max(farthest, depth);
    }
    ReducedDepth[id] = farthest;
}

bool IsOccluded(ShadowDrawArgs command)
{
    // Directional projections are orthographic, so their homogeneous w is one.
    InstanceData instance = Instances[command.InstanceID];
    ShadowView view = Views[command.ShadowViewIndex];
    float3 center = mul(float4(instance.BoundingBoxCenter, 1), view.ViewProj).xyz;
    float3 extent = float3(
        dot(abs(float3(view.ViewProj._11, view.ViewProj._21, view.ViewProj._31)), instance.BoundingBoxExtents),
        dot(abs(float3(view.ViewProj._12, view.ViewProj._22, view.ViewProj._32)), instance.BoundingBoxExtents),
        dot(abs(float3(view.ViewProj._13, view.ViewProj._23, view.ViewProj._33)), instance.BoundingBoxExtents));
    float nearest = center.z - extent.z;
    // AABB crossing the light near plane is kept. Inflate XY for rasterization precision;
    // use a strict depth margin so touching/coplanar surfaces are never culled.
    if (nearest <= 0) return false;
    float2 uvMin = float2(center.x - extent.x, -center.y - extent.y) * 0.5f + 0.5f;
    float2 uvMax = float2(center.x + extent.x, -center.y + extent.y) * 0.5f + 0.5f;
    uint2 low = (uint2)clamp(floor(uvMin * ShadowMapResolution - 1), 0, ShadowMapResolution - 1);
    uint2 high = (uint2)clamp(ceil(uvMax * ShadowMapResolution + 1), 0, ShadowMapResolution - 1);
    low >>= 1;
    high >>= 1;
    uint width, height, arrays, mipCount;
    Depth.GetDimensions(0, width, height, arrays, mipCount);
    uint mip = 0;
    while (any(high - low > 1) && mip + 1 < mipCount)
    {
        low >>= 1;
        high >>= 1;
        ++mip;
    }
    float farthest = max(max(Depth.Load(int4(low, view.Layer, mip)), Depth.Load(int4(high, view.Layer, mip))),
                         max(Depth.Load(int4(low.x, high.y, view.Layer, mip)), Depth.Load(int4(high.x, low.y, view.Layer, mip))));
    return nearest > farthest + 1e-5f;
}

[numthreads(64, 1, 1)]
void CullCS(uint3 id : SV_DispatchThreadID)
{
    uint index = DrawOffset + id.x;
    if (index >= CandidateCount[0]) return;
    ShadowDrawArgs command = Candidates[index];
    // Occluders have already been drawn and must not be submitted a second time.
    if (IsOccluder(command) || IsOccluded(command)) return;
    uint offset;
    InterlockedAdd(Counts[1], 1, offset);
    Visible[offset] = command;
}
