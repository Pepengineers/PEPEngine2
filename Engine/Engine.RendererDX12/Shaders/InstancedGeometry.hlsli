#ifndef INSTANCED_GEOMETRY_HLSLI
#define INSTANCED_GEOMETRY_HLSLI

ByteAddressBuffer SkinVertices : register(t0, space1);
StructuredBuffer<uint> VisibleSkinInstances : register(t1, space1);

uint DrawInstance(uint rootId, uint instanceId, InstanceData root)
{
    [branch]
    if (root.SkinBatch == 0xffffffffu) return rootId;
    return VisibleSkinInstances[root.SkinBatchStart + instanceId];
}

struct SkinVertex
{
    float3 Position;
    float3 Normal;
    float2 UV;
    float3 Tangent;
    float3 PreviousPosition;
};

SkinVertex ReadSkinVertex(InstanceData instance, uint vertexId)
{
    uint address = (instance.SkinOutputVertex + vertexId) * 56;
    SkinVertex vertex;
    vertex.Position = asfloat(SkinVertices.Load3(address));
    vertex.Normal = asfloat(SkinVertices.Load3(address + 12));
    vertex.UV = asfloat(SkinVertices.Load2(address + 24));
    vertex.Tangent = asfloat(SkinVertices.Load3(address + 32));
    vertex.PreviousPosition = asfloat(SkinVertices.Load3(address + 44));
    return vertex;
}

#endif
