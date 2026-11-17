#include "CBufferStructures.hlsl"

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
#ifndef SHADOW_DEPTH_ONLY
    float2 UV : TEXCOORD0;
    nointerpolation uint MaterialIndex : TEXCOORD1;
    nointerpolation uint CastsShadows : TEXCOORD2;
#endif
};

VertexOutput VS(float3 position : POSITION, float2 uv : TEXCOORD, uint vertexID : SV_VertexID, uint instanceID : SV_InstanceID)
{
#ifdef SHADOW_UNBATCHED
    InstanceData instance = Instances[InstanceID];
#else
    InstanceData instance = Instances[DrawInstance(InstanceID, instanceID, Instances[InstanceID])];
#endif
    if (instance.SkinOutputVertex != 0xffffffffu)
    {
        SkinVertex vertex = ReadSkinVertex(instance, vertexID);
        position = vertex.Position;
        uv = vertex.UV;
    }
    VertexOutput output;
    float4 world = mul(float4(position, 1), Transforms[instance.TransformIndex].World);
    output.Position = mul(world, Lights[LightFaceIndex / 6].ShadowViewProj[LightFaceIndex % 6]);
#ifndef SHADOW_DEPTH_ONLY
    output.UV = uv;
    output.MaterialIndex = instance.MaterialIndex;
    output.CastsShadows = instance.CastsShadows;
#endif
    return output;
}

#ifndef SHADOW_DEPTH_ONLY
void PS(VertexOutput input)
{
    if (input.CastsShadows == 0) clip(-1.0f);
    Material material = Materials[input.MaterialIndex];
    if (material.ReflectionProbePreview != 0) clip(-1.0f);
    if (material.RenderLayer == 1) clip(Textures[material.DiffuseIndex].Sample(samAnisotropicWrap, input.UV).a - 0.5f);
}
#endif
