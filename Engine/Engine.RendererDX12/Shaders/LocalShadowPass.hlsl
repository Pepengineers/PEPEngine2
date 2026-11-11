#include "CBufferStructures.hlsl"

#ifdef SHADOW_CULLING
#include "ShadowFrustum.hlsli"
cbuffer ViewConstants : register(b0) { uint LightIndex; uint FaceIndex; uint CommandCount; };
StructuredBuffer<InstanceData> Instances : register(t0);
StructuredBuffer<IndirectDrawArgs> InputCommands : register(t1);
StructuredBuffer<Material> Materials : register(t2);
StructuredBuffer<LocalLight> Lights : register(t3);
RWStructuredBuffer<IndirectDrawArgs> VisibleCommands : register(u0);
RWStructuredBuffer<uint> Counter : register(u1);

[numthreads(1, 1, 1)]
void ClearCS() { Counter[0] = 0; }

[numthreads(64, 1, 1)]
void CS(uint3 id : SV_DispatchThreadID)
{
    uint count, stride;
    InputCommands.GetDimensions(count, stride);
    if (id.x >= min(count, CommandCount)) return;
    IndirectDrawArgs command = InputCommands[id.x];
    if (command.InstanceCount == 0) return;
    InstanceData instance = Instances[command.InstanceID];
    if (instance.CastsShadows == 0) return;
#if STATIC_ONLY
    if (instance.ReflectionDynamic != 0) return;
#else
    if (instance.ReflectionDynamic == 0) return;
#endif
    Material material = Materials[instance.MaterialIndex];
    if (material.RenderLayer == 2 || material.ReflectionProbePreview != 0) return;
    if (!ShadowBoxVisible(instance.BoundingBoxCenter, instance.BoundingBoxExtents, Lights[LightIndex].ShadowViewProj[FaceIndex])) return;
    uint index;
    InterlockedAdd(Counter[0], 1, index);
    VisibleCommands[index] = command;
}
#else
#include "InstancedGeometry.hlsli"
cbuffer DrawConstants : register(b0) { uint InstanceID; uint LightFaceIndex; };
StructuredBuffer<Material> Materials : register(t0);
StructuredBuffer<Transform> Transforms : register(t1);
StructuredBuffer<InstanceData> Instances : register(t2);
StructuredBuffer<LocalLight> Lights : register(t3);
Texture2D Textures[TEXTURE2D_RANGE_LENGTH] : register(t4);
SamplerState samAnisotropicWrap : register(s4);

struct VertexOutput
{
    float4 Position : SV_POSITION;
    float2 UV : TEXCOORD0;
    nointerpolation uint MaterialIndex : TEXCOORD1;
    nointerpolation uint CastsShadows : TEXCOORD2;
};

VertexOutput VS(float3 position : POSITION, float2 uv : TEXCOORD, uint vertexID : SV_VertexID, uint instanceID : SV_InstanceID)
{
    InstanceData instance = Instances[DrawInstance(InstanceID, instanceID, Instances[InstanceID])];
    if (instance.SkinOutputVertex != 0xffffffffu)
    {
        SkinVertex vertex = ReadSkinVertex(instance, vertexID);
        position = vertex.Position;
        uv = vertex.UV;
    }
    VertexOutput output;
    float4 world = mul(float4(position, 1), Transforms[instance.TransformIndex].World);
    output.Position = mul(world, Lights[LightFaceIndex / 6].ShadowViewProj[LightFaceIndex % 6]);
    output.UV = uv;
    output.MaterialIndex = instance.MaterialIndex;
    output.CastsShadows = instance.CastsShadows;
    return output;
}

void PS(VertexOutput input)
{
    if (input.CastsShadows == 0) clip(-1.0f);
    Material material = Materials[input.MaterialIndex];
    if (material.ReflectionProbePreview != 0) clip(-1.0f);
    if (material.RenderLayer == 1) clip(Textures[material.DiffuseIndex].Sample(samAnisotropicWrap, input.UV).a - 0.5f);
}
#endif
