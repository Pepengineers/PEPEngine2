// Unmodified pinned FidelityFX Parallel Sort kernels, with engine buffer callbacks.
cbuffer Pass : register(b0) { uint Shift; uint Stage; };
cbuffer SortParameters : register(b1)
{
    uint NumKeys;
    uint BlocksPerGroup;
    uint NumGroups;
    uint ExtraGroups;
    uint ReduceGroupsPerBin;
    uint ScanValues;
    uint2 Padding;
};
RWStructuredBuffer<uint> SourceKeys : register(u0);
RWStructuredBuffer<uint> DestKeys : register(u1);
RWStructuredBuffer<uint> SourcePayload : register(u2);
RWStructuredBuffer<uint> DestPayload : register(u3);
RWStructuredBuffer<uint> Sum : register(u4);
RWStructuredBuffer<uint> Reduced : register(u5);

#define FFX_GPU 1
#define FFX_HLSL 1
#define FFX_PARALLELSORT_COPY_VALUE 1
#define FfxUInt32 uint
#define FfxInt32 int
#define FFX_GROUPSHARED groupshared
#define FFX_GROUP_MEMORY_BARRIER GroupMemoryBarrierWithGroupSync()
#define FFX_ATOMIC_ADD(destination, value) InterlockedAdd(destination, value)

uint FfxNumBlocksPerThreadGroup() { return BlocksPerGroup; }
uint FfxNumThreadGroups() { return NumGroups; }
uint FfxNumThreadGroupsWithAdditionalBlocks() { return ExtraGroups; }
uint FfxNumReduceThreadgroupPerBin() { return ReduceGroupsPerBin; }
uint FfxNumKeys() { return NumKeys; }
uint FfxLoadKey(uint index) { return SourceKeys[index]; }
void FfxStoreKey(uint index, uint value) { DestKeys[index] = value; }
uint FfxLoadPayload(uint index) { return SourcePayload[index]; }
void FfxStorePayload(uint index, uint value) { DestPayload[index] = value; }
uint FfxLoadSum(uint index) { return Sum[index]; }
void FfxStoreSum(uint index, uint value) { Sum[index] = value; }
void FfxStoreReduce(uint index, uint value) { Reduced[index] = value; }
uint FfxLoadScanSource(uint index) { return Stage == 2 ? Reduced[index] : Sum[index]; }
void FfxStoreScanDest(uint index, uint value)
{
    if (Stage == 2) Reduced[index] = value;
    else Sum[index] = value;
}
uint FfxLoadScanScratch(uint index) { return Reduced[index]; }

#include "../../../External/Particles/ffx_parallelsort.h"

[numthreads(128, 1, 1)]
void Count(uint lane : SV_GroupIndex, uint group : SV_GroupID)
{
    ffxParallelSortCountUInt(lane, group, Shift);
}
[numthreads(128, 1, 1)]
void Reduce(uint lane : SV_GroupIndex, uint group : SV_GroupID)
{
    ffxParallelSortReduceCount(lane, group);
}
[numthreads(128, 1, 1)]
void Scan(uint lane : SV_GroupIndex)
{
    ffxParallelSortScanPrefix(ScanValues, lane, 0, 0, 0, false);
}
[numthreads(128, 1, 1)]
void ScanAdd(uint lane : SV_GroupIndex, uint group : SV_GroupID)
{
    uint bin = group / ReduceGroupsPerBin;
    uint base = (group % ReduceGroupsPerBin) * 512;
    ffxParallelSortScanPrefix(NumGroups, lane, group, bin * NumGroups, base, true);
}
[numthreads(128, 1, 1)]
void Scatter(uint lane : SV_GroupIndex, uint group : SV_GroupID)
{
    ffxParallelSortScatterUInt(lane, group, Shift);
}
