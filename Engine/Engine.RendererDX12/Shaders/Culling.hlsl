#include "CBufferStructures.hlsl"

ConstantBuffer<CameraCB> CBCamera : register(b0);

StructuredBuffer<InstanceData> InstanceCache : register(t0);
StructuredBuffer<IndirectDrawArgs> InputCommands : register(t1);
AppendStructuredBuffer<IndirectDrawArgs> VisibleCommands : register(u0);

[numthreads(64, 1, 1)]
void CS(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint drawIndex = dispatchThreadID.x;
    
    uint numDraws, stride;
    InputCommands.GetDimensions(numDraws, stride);
    
    if (drawIndex >= numDraws)
        return;
    
    // TODO: add actual culling
    // Just copy all commands for now
    VisibleCommands.Append(InputCommands[drawIndex]);
}