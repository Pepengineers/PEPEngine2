// FidelityFX Parallel Sort v1.1.4 integration; SDK pin c6efa6bf7f2027b3ec94f28578bb5965eabb9e55.
// Copyright (C) 2024 Advanced Micro Devices, Inc.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#ifdef SORT_SETUP
cbuffer SetupParameters : register(b0) { uint Capacity; uint CountIndex; };
StructuredBuffer<uint> Count : register(t0);
RWStructuredBuffer<uint> Constants : register(u0);
RWStructuredBuffer<uint> Arguments : register(u1);
[numthreads(1,1,1)]
void SetupCS()
{
    uint n=min(Count[CountIndex],Capacity);
    uint blocks=(n+511)/512,groups=min(blocks,800);
    uint blocksPerGroup=groups?blocks/groups:0,additional=groups?blocks%groups:0;
    uint reducePerBin=(groups+511)/512,reduce=16*reducePerBin;
    for(uint pass=0;pass<8;++pass)
    {
        uint base=pass*64;
        Constants[base]=n; Constants[base+1]=blocksPerGroup; Constants[base+2]=groups;
        Constants[base+3]=additional; Constants[base+4]=reducePerBin; Constants[base+5]=reduce;
        Constants[base+6]=pass*4; Constants[base+7]=0;
    }
    Arguments[0]=groups; Arguments[1]=1; Arguments[2]=1;
    Arguments[3]=reduce; Arguments[4]=1; Arguments[5]=1;
    Arguments[6]=n?1:0; Arguments[7]=1; Arguments[8]=1;
}
#else
#define FFX_GPU 1
#define FFX_HLSL 1
#define FFX_PARALLELSORT_COPY_VALUE 1
#include "../../../Build/FidelityFXSource/sdk/include/FidelityFX/gpu/ffx_core.h"

cbuffer SortParameters : register(b0)
{
    uint KeyCount; uint BlocksPerGroup; uint GroupCount; uint AdditionalGroups;
    uint ReduceGroupsPerBin; uint ScanValues; uint ShiftBits; uint Padding;
};
RWStructuredBuffer<uint> SourceKeys : register(u0);
RWStructuredBuffer<uint> DestKeys : register(u1);
RWStructuredBuffer<uint> SourcePayloads : register(u2);
RWStructuredBuffer<uint> DestPayloads : register(u3);
RWStructuredBuffer<uint> Sum : register(u4);
RWStructuredBuffer<uint> Reduced : register(u5);

FfxUInt32 FfxNumBlocksPerThreadGroup() { return BlocksPerGroup; }
FfxUInt32 FfxNumThreadGroups() { return GroupCount; }
FfxUInt32 FfxNumThreadGroupsWithAdditionalBlocks() { return AdditionalGroups; }
FfxUInt32 FfxNumReduceThreadgroupPerBin() { return ReduceGroupsPerBin; }
FfxUInt32 FfxNumKeys() { return KeyCount; }
FfxUInt32 FfxLoadKey(FfxUInt32 i) { return SourceKeys[i]; }
void FfxStoreKey(FfxUInt32 i,FfxUInt32 value) { DestKeys[i]=value; }
FfxUInt32 FfxLoadPayload(FfxUInt32 i) { return SourcePayloads[i]; }
void FfxStorePayload(FfxUInt32 i,FfxUInt32 value) { DestPayloads[i]=value; }
FfxUInt32 FfxLoadSum(FfxUInt32 i) { return Sum[i]; }
void FfxStoreSum(FfxUInt32 i,FfxUInt32 value) { Sum[i]=value; }
void FfxStoreReduce(FfxUInt32 i,FfxUInt32 value) { Reduced[i]=value; }
#ifdef SORT_SCAN_ADD
FfxUInt32 FfxLoadScanSource(FfxUInt32 i) { return Sum[i]; }
void FfxStoreScanDest(FfxUInt32 i,FfxUInt32 value) { Sum[i]=value; }
#else
FfxUInt32 FfxLoadScanSource(FfxUInt32 i) { return Reduced[i]; }
void FfxStoreScanDest(FfxUInt32 i,FfxUInt32 value) { Reduced[i]=value; }
#endif
FfxUInt32 FfxLoadScanScratch(FfxUInt32 i) { return Reduced[i]; }
#pragma dxc diagnostic push
#pragma dxc diagnostic ignored "-Wambig-lit-shift"
#include "../../../Build/FidelityFXSource/sdk/include/FidelityFX/gpu/parallelsort/ffx_parallelsort.h"
#pragma dxc diagnostic pop

[numthreads(128,1,1)]
void CountCS(uint local : SV_GroupThreadID,uint group : SV_GroupID) { ffxParallelSortCountUInt(local,group,ShiftBits); }
[numthreads(128,1,1)]
void ReduceCS(uint local : SV_GroupThreadID,uint group : SV_GroupID) { ffxParallelSortReduceCount(local,group); }
[numthreads(128,1,1)]
void ScanCS(uint local : SV_GroupThreadID,uint group : SV_GroupID) { ffxParallelSortScanPrefix(ScanValues,local,group,0,group*512,false); }
[numthreads(128,1,1)]
void ScanAddCS(uint local : SV_GroupThreadID,uint group : SV_GroupID)
{
    uint bin=group/ReduceGroupsPerBin,base=(group%ReduceGroupsPerBin)*512;
    ffxParallelSortScanPrefix(GroupCount,local,group,bin*GroupCount,base,true);
}
[numthreads(128,1,1)]
void ScatterCS(uint local : SV_GroupThreadID,uint group : SV_GroupID) { ffxParallelSortScatterUInt(local,group,ShiftBits); }
#endif
