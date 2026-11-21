#include "CBufferStructures.hlsli"

ConstantBuffer<CameraCB> CBCamera : register(b0);

StructuredBuffer<InstanceData> InstanceCache : register(t0);
StructuredBuffer<IndirectDrawArgs> InputCommands : register(t1);
StructuredBuffer<Material> MaterialCache : register(t2);
RWStructuredBuffer<IndirectDrawArgs> OpaqueVisibleCommands : register(u0);
RWStructuredBuffer<uint> OpaqueCounterBuffer : register(u1);
RWStructuredBuffer<IndirectDrawArgs> TransparentVisibleCommands : register(u2);
RWStructuredBuffer<uint> TransparentCounterBuffer : register(u3);

[numthreads(64, 1, 1)]
void CS(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint drawIndex = dispatchThreadID.x;
    
    uint numDraws, stride;
    InputCommands.GetDimensions(numDraws, stride);
    
    if (drawIndex >= numDraws)
        return;
    
    IndirectDrawArgs command = InputCommands[drawIndex];
    if (command.InstanceCount == 0)
        return;

    uint instanceID = command.StartInstanceLocation;
    
    InstanceData instance = InstanceCache[instanceID];
    if (IsBoxVisible(instance.BoundingBoxCenter, instance.BoundingBoxExtents, CBCamera.ViewProj))
    {
        Material mat = MaterialCache[InstanceCache[instanceID].MaterialIndex];
        
        if (mat.RenderLayer != RENDER_LAYER_TRANSPARENT)
        {
            uint writeIndex;
            InterlockedAdd(OpaqueCounterBuffer[0], 1, writeIndex);
            OpaqueVisibleCommands[writeIndex] = command;
        }
        else
        {
            uint writeIndex;
            InterlockedAdd(TransparentCounterBuffer[0], 1, writeIndex);
            TransparentVisibleCommands[writeIndex] = command;
        }
    }
}