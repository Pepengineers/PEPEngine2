#include "CBufferStructures.hlsl"

cbuffer Parameters : register(b0) { uint DrawCapacity; uint BatchCount; uint CounterOffset; };
StructuredBuffer<InstanceData> Instances : register(t0);
StructuredBuffer<IndirectDrawArgs> InputCommands : register(t1);
RWStructuredBuffer<IndirectDrawArgs> Groups : register(u0);
RWStructuredBuffer<IndirectDrawArgs> PackedCommands : register(u1);
RWStructuredBuffer<uint> Counter : register(u2);
RWStructuredBuffer<uint> VisibleInstances : register(u3);
RWStructuredBuffer<uint> SourceCount : register(u4);

uint ThreadIndex(uint3 group, uint lane)
{
    return (group.y * 65535 + group.x) * 64 + lane;
}

[numthreads(64, 1, 1)]
void ClearCS(uint3 group : SV_GroupID, uint lane : SV_GroupIndex)
{
    uint index = ThreadIndex(group, lane);
    if (index < BatchCount) Groups[index].InstanceCount = 0;
    if (index == 0)
    {
        SourceCount[0] = Counter[CounterOffset];
        Counter[CounterOffset] = 0;
    }
}

[numthreads(64, 1, 1)]
void AppendCS(uint3 group : SV_GroupID, uint lane : SV_GroupIndex)
{
    uint index = ThreadIndex(group, lane);
    if (index >= DrawCapacity || index >= SourceCount[0]) return;
    IndirectDrawArgs command = InputCommands[index];
    if (command.InstanceCount == 0) return;
    InstanceData instance = Instances[command.InstanceID];
    uint writeIndex;
    if (instance.SkinBatch == 0xffffffffu)
    {
        InterlockedAdd(Counter[CounterOffset], 1, writeIndex);
        PackedCommands[writeIndex] = command;
        return;
    }
    uint batch = instance.SkinBatch;
    InterlockedAdd(Groups[batch].InstanceCount, 1, writeIndex);
    VisibleInstances[instance.SkinBatchStart + writeIndex] = command.InstanceID;
    if (writeIndex == 0)
    {
        // The count has concurrent atomic writers; only initialize the remaining fields.
        Groups[batch].InstanceID = command.InstanceID;
        Groups[batch].IndexCountPerInstance = command.IndexCountPerInstance;
        Groups[batch].StartIndexLocation = command.StartIndexLocation;
        Groups[batch].BaseVertexLocation = command.BaseVertexLocation;
        Groups[batch].StartInstanceLocation = 0;
    }
}

[numthreads(64, 1, 1)]
void FinalizeCS(uint3 group : SV_GroupID, uint lane : SV_GroupIndex)
{
    uint batch = ThreadIndex(group, lane);
    if (batch >= BatchCount || Groups[batch].InstanceCount == 0) return;
    uint writeIndex;
    InterlockedAdd(Counter[CounterOffset], 1, writeIndex);
    PackedCommands[writeIndex] = Groups[batch];
}
