#include "CBufferStructures.hlsl"

ConstantBuffer<CameraCB> CBCamera : register(b0);

StructuredBuffer<InstanceData> InstanceCache : register(t0);
StructuredBuffer<IndirectDrawArgs> InputCommands : register(t1);
StructuredBuffer<Material> MaterialCache : register(t3);
RWStructuredBuffer<IndirectDrawArgs> VisibleCommands : register(u0);
RWStructuredBuffer<uint> CounterBuffer : register(u1);

bool IsBoxVisible(uint instanceIndex)
{
    InstanceData inst = InstanceCache[instanceIndex];
    
    float3 center = inst.BoundingBoxCenter;
    float3 extents = inst.BoundingBoxExtents;
    
    float4x4 m = CBCamera.ViewProj;
    
    float4 planes[6];
    planes[0] = float4(m._14 + m._11, m._24 + m._21, m._34 + m._31, m._44 + m._41);
    planes[1] = float4(m._14 - m._11, m._24 - m._21, m._34 - m._31, m._44 - m._41);
    planes[2] = float4(m._14 + m._12, m._24 + m._22, m._34 + m._32, m._44 + m._42);
    planes[3] = float4(m._14 - m._12, m._24 - m._22, m._34 - m._32, m._44 - m._42);
    planes[4] = float4(m._13, m._23, m._33, m._43);
    planes[5] = float4(m._14 - m._13, m._24 - m._23, m._34 - m._33, m._44 - m._43);
    
    for (int i = 0; i < 6; i++)
    {
        float r = extents.x * abs(planes[i].x) + extents.y * abs(planes[i].y) + extents.z * abs(planes[i].z);
        float d = dot(float4(center, 1.0f), planes[i]);
        
        if (d + r < 0.0f)
            return false;
    }
    return true;
}

[numthreads(64, 1, 1)]
void CS(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint drawIndex = dispatchThreadID.x;
    
    uint numDraws, stride;
    InputCommands.GetDimensions(numDraws, stride);
    
    if (drawIndex >= numDraws)
        return;
    
    if (IsBoxVisible(InputCommands[drawIndex].StartInstanceLocation))
    {
        uint writeIndex;
        InterlockedAdd(CounterBuffer[0], 1, writeIndex);
        VisibleCommands[writeIndex] = InputCommands[drawIndex];
    }
}