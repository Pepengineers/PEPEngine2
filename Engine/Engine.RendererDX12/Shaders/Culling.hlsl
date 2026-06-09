#include "CBufferStructures.hlsl"

ConstantBuffer<CameraCB> CBCamera : register(b0);

StructuredBuffer<InstanceData> InstanceCache : register(t0);
StructuredBuffer<IndirectDrawArgs> InputCommands : register(t1);
RWStructuredBuffer<IndirectDrawArgs> VisibleCommands : register(u0);
RWStructuredBuffer<uint> CounterBuffer : register(u1);

[numthreads(64, 1, 1)]
void CS(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint drawIndex = dispatchThreadID.x;
    
    uint numDraws, stride;
    InputCommands.GetDimensions(numDraws, stride);
    
    if (drawIndex >= numDraws)
        return;
    
    if (drawIndex == 0)
    {
        CounterBuffer[0] = 0;
    }
    
    GroupMemoryBarrierWithGroupSync();
    
    
    // TODO: add actual culling
    if(true)
    {
        uint writeIndex;
        InterlockedAdd(CounterBuffer[0], 1, writeIndex);
        VisibleCommands[writeIndex] = InputCommands[drawIndex];
    }
}