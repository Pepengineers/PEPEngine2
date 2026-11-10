#include "CBufferStructures.hlsl"

#ifdef SHADOW_CULLING
ConstantBuffer<ShadowCB> CBShadow : register(b0);
StructuredBuffer<IndirectDrawArgs> InputCommands : register(t1);
RWStructuredBuffer<IndirectDrawArgs> Commands0 : register(u0);
RWStructuredBuffer<IndirectDrawArgs> Commands1 : register(u1);
RWStructuredBuffer<IndirectDrawArgs> Commands2 : register(u2);
RWStructuredBuffer<IndirectDrawArgs> Commands3 : register(u3);
RWStructuredBuffer<uint> Counters : register(u4);
StructuredBuffer<Material> MaterialCache : register(t2);
StructuredBuffer<InstanceData> InstanceCache : register(t0);

[numthreads(1, 1, 1)]
void ClearCS()
{
    for (uint cascade = 0; cascade < 4; ++cascade) Counters[cascade] = 0;
}

bool Visible(InstanceData instance, float4x4 m)
{
    float4 planes[6] = {
        float4(m._14 + m._11, m._24 + m._21, m._34 + m._31, m._44 + m._41),
        float4(m._14 - m._11, m._24 - m._21, m._34 - m._31, m._44 - m._41),
        float4(m._14 + m._12, m._24 + m._22, m._34 + m._32, m._44 + m._42),
        float4(m._14 - m._12, m._24 - m._22, m._34 - m._32, m._44 - m._42),
        float4(m._13, m._23, m._33, m._43),
        float4(m._14 - m._13, m._24 - m._23, m._34 - m._33, m._44 - m._43) };
    for (uint p = 0; p < 6; ++p)
        if (dot(float4(instance.BoundingBoxCenter, 1), planes[p]) + dot(instance.BoundingBoxExtents, abs(planes[p].xyz)) < 0)
            return false;
    return true;
}

[numthreads(64, 1, 1)]
void CS(uint3 id : SV_DispatchThreadID)
{
    uint count, stride;
    InputCommands.GetDimensions(count, stride);
    if (id.x >= count) return;
    IndirectDrawArgs command = InputCommands[id.x];
    InstanceData instance = InstanceCache[command.InstanceID];
    Material material = MaterialCache[instance.MaterialIndex];
    if (material.RenderLayer == 2 || material.ReflectionProbePreview != 0) return;
    for (uint cascade = 0; cascade < 4; ++cascade)
    {
        if (!Visible(instance, CBShadow.ViewProj[cascade])) continue;
        uint index;
        InterlockedAdd(Counters[cascade], 1, index);
        if (cascade == 0) Commands0[index] = command;
        else if (cascade == 1) Commands1[index] = command;
        else if (cascade == 2) Commands2[index] = command;
        else Commands3[index] = command;
    }
}
#else
#ifdef SHADOW_REPLICA_IA
cbuffer DrawConstants : register(b0) { uint InstanceID; uint3 Padding; float4x4 ReplicaViewProj; };
#else
#include "InstancedGeometry.hlsli"
cbuffer DrawConstants : register(b0) { uint InstanceID; uint CascadeIndex; };
ConstantBuffer<ShadowCB> CBShadow : register(b1);
#endif
StructuredBuffer<Material> MaterialCache : register(t0);
StructuredBuffer<Transform> TransformCache : register(t1);
StructuredBuffer<InstanceData> InstanceCache : register(t2);
Texture2D Texture2DCache[TEXTURE2D_RANGE_LENGTH] : register(t3);
SamplerState samAnisotropicWrap : register(s4);

struct VertexOutput
{
    float4 Position : SV_POSITION;
    float2 UV : TEXCOORD0;
    nointerpolation uint MaterialIndex : TEXCOORD1;
};

VertexOutput VS(float3 position : POSITION, float2 uv : TEXCOORD, uint vertexID : SV_VertexID, uint instanceID : SV_InstanceID)
{
#ifdef SHADOW_REPLICA_IA
    InstanceData instance = InstanceCache[InstanceID];
#else
    InstanceData instance = InstanceCache[DrawInstance(InstanceID, instanceID, InstanceCache[InstanceID])];
    if (instance.SkinOutputVertex != 0xffffffffu)
    {
        SkinVertex vertex = ReadSkinVertex(instance, vertexID);
        position = vertex.Position;
        uv = vertex.UV;
    }
#endif
    VertexOutput output;
    float4 world = mul(float4(position, 1), TransformCache[instance.TransformIndex].World);
#ifdef SHADOW_REPLICA_IA
    output.Position = mul(world, ReplicaViewProj);
#else
    output.Position = mul(world, CBShadow.ViewProj[CascadeIndex]);
#endif
    output.UV = uv;
    output.MaterialIndex = instance.MaterialIndex;
    return output;
}

void PS(VertexOutput input)
{
    Material material = MaterialCache[input.MaterialIndex];
#ifdef SHADOW_REPLICA_IA
    if (material.ReflectionProbePreview != 0) clip(-1.0f);
#endif
    if (material.RenderLayer == 1)
        clip(Texture2DCache[material.DiffuseIndex].Sample(samAnisotropicWrap, input.UV).a - 0.5f);
}
#endif
