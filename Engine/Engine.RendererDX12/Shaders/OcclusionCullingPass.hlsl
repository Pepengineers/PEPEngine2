#include "CBufferStructures.hlsli"

cbuffer Parameters : register(b0)
{
    uint2 RenderSize;
    uint2 TargetSize;
    uint DrawOffset;
    uint FirstReduction;
    float MinOccluderSizePixels;
    uint Padding;
};
ConstantBuffer<CameraCB> CBCamera : register(b1);
StructuredBuffer<IndirectDrawArgs> Candidates : register(t0);
StructuredBuffer<uint> CandidateCount : register(t1);
StructuredBuffer<InstanceData> Instances : register(t2);
StructuredBuffer<Material> Materials : register(t3);
Texture2D<float> Depth : register(t4);
RWStructuredBuffer<IndirectDrawArgs> Commands : register(u0);
RWStructuredBuffer<uint> Count : register(u1);
RWTexture2D<float> ReducedDepth : register(u2);
RWStructuredBuffer<uint> ClearOpaqueCount : register(u3);
RWStructuredBuffer<uint> ClearTransparentCount : register(u4);

// Positive homogeneous w makes vertex extrema conservative for a perspective AABB.
// A box crossing the eye plane is kept; for seed selection it is considered large.
bool ProjectBounds(IndirectDrawArgs command, out float2 low, out float2 high, out float nearest)
{
    InstanceData instance = Instances[command.InstanceID];
    low = 3.402823e38f;
    high = -3.402823e38f;
    nearest = 0;
    [unroll] for (uint i = 0; i < 8; ++i)
    {
        float3 sign = float3(i & 1 ? 1 : -1, i & 2 ? 1 : -1, i & 4 ? 1 : -1);
        float3 corner = instance.BoundingBoxCenter + sign * instance.BoundingBoxExtents;
        float4 clip = mul(float4(corner, 1), CBCamera.ViewProj);
        if (clip.w <= 1e-5f) return false;
        float3 ndc = clip.xyz / clip.w;
        float2 uv = ndc.xy * float2(.5f, -.5f) + .5f;
        low = min(low, uv);
        high = max(high, uv);
        nearest = max(nearest, ndc.z);
    }
    return true;
}

bool IsLargeEnough(float2 low, float2 high)
{
    float2 size = (high - low) * RenderSize;
    return min(size.x, size.y) >= MinOccluderSizePixels;
}

bool IsOccluder(IndirectDrawArgs command)
{
    if (Materials[Instances[command.InstanceID].MaterialIndex].RenderLayer != RENDER_LAYER_OPAQUE) return false;
    float2 low, high;
    float nearest;
    if (!ProjectBounds(command, low, high, nearest)) return true;
    return IsLargeEnough(low, high);
}

[numthreads(1, 1, 1)]
void ClearCS(uint3 id : SV_DispatchThreadID)
{
    Count[0] = 0;
    ClearOpaqueCount[0] = 0;
    ClearTransparentCount[0] = 0;
}

[numthreads(64, 1, 1)]
void SelectOccludersCS(uint3 id : SV_DispatchThreadID)
{
    uint index = DrawOffset + id.x;
    if (index >= CandidateCount[0]) return;
    IndirectDrawArgs command = Candidates[index];
    if (!IsOccluder(command)) return;
    uint offset;
    InterlockedAdd(Count[0], 1, offset);
    Commands[offset] = command;
}

// Camera depth is reversed-Z: clear=0, GREATER_EQUAL, and conservative min reduction.
// Power-of-two padding remains uncovered. Clamped reads handle rectangular mip tails.
[numthreads(8, 8, 1)]
void ReduceCS(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= TargetSize)) return;
    uint width, height;
    Depth.GetDimensions(width, height);
    float farthest = 1;
    for (uint y = 0; y < 2; ++y)
    for (uint x = 0; x < 2; ++x)
    {
        uint2 pixel = id.xy * 2 + uint2(x, y);
        float depth = FirstReduction && any(pixel >= RenderSize) ? 0 :
            Depth.Load(int3(min(pixel, uint2(width, height) - 1), 0));
        farthest = min(farthest, depth);
    }
    ReducedDepth[id.xy] = farthest;
}

bool IsOccluded(float2 low, float2 high, float nearest)
{
    if (nearest >= 1) return false;
    uint2 first = (uint2)clamp(floor(low * RenderSize - 1), 0, RenderSize - 1);
    uint2 last = (uint2)clamp(ceil(high * RenderSize + 1), 0, RenderSize - 1);
    first >>= 1;
    last >>= 1;
    uint width, height, mipCount;
    Depth.GetDimensions(0, width, height, mipCount);
    uint mip = 0;
    while (any(last - first > 1) && mip + 1 < mipCount)
    {
        first >>= 1;
        last >>= 1;
        ++mip;
    }
    float farthest = min(min(Depth.Load(int3(first, mip)), Depth.Load(int3(last, mip))),
                         min(Depth.Load(int3(first.x, last.y, mip)), Depth.Load(int3(last.x, first.y, mip))));
    return nearest < farthest - 1e-5f;
}

[numthreads(64, 1, 1)]
void CullCS(uint3 id : SV_DispatchThreadID)
{
    uint index = DrawOffset + id.x;
    if (index >= CandidateCount[0]) return;
    IndirectDrawArgs command = Candidates[index];
    // Seed depth is private, so its meshes still need their normal G-buffer draws.
    float2 low, high;
    float nearest;
    if (ProjectBounds(command, low, high, nearest))
    {
        bool seed = Materials[Instances[command.InstanceID].MaterialIndex].RenderLayer == RENDER_LAYER_OPAQUE &&
                    IsLargeEnough(low, high);
        if (!seed && IsOccluded(low, high, nearest)) return;
    }
    uint offset;
    InterlockedAdd(Count[0], 1, offset);
    Commands[offset] = command;
}
