#include "CBufferStructures.hlsli"

cbuffer DrawConstants : register(b0)
{
    uint InstanceID;
    uint ShadowViewIndex;
};
StructuredBuffer<Material> MaterialCache : register(t0);
StructuredBuffer<Transform> TransformCache : register(t1);
StructuredBuffer<InstanceData> InstanceCache : register(t2);
StructuredBuffer<ShadowView> ShadowViews : register(t3);
Texture2D Texture2DCache[TEXTURE2D_RANGE_LENGTH] : register(t4);
SamplerState samAnisotropicWrap : register(s4);

struct VS_OUTPUT
{
    float4 Position : SV_POSITION;
    float2 TexC : TEXCOORD0;
    nointerpolation uint MaterialIndex : TEXCOORD1;
#if SHADOW_LAYER_FROM_VS
    uint Layer : SV_RenderTargetArrayIndex;
#else
    nointerpolation uint Layer : TEXCOORD2;
#endif
};

VS_OUTPUT VS(float3 position : POSITION, float2 texC : TEXCOORD)
{
    InstanceData instance = InstanceCache[InstanceID];
    ShadowView view = ShadowViews[ShadowViewIndex];
    VS_OUTPUT output;
    float4 world = mul(float4(position, 1), TransformCache[instance.TransformIndex].World);
    output.Position = mul(world, view.ViewProj);
    output.TexC = texC;
    output.MaterialIndex = instance.MaterialIndex;
    output.Layer = view.Layer;
    return output;
}

#if !SHADOW_LAYER_FROM_VS
struct GS_OUTPUT
{
    float4 Position : SV_POSITION;
    float2 TexC : TEXCOORD0;
    nointerpolation uint MaterialIndex : TEXCOORD1;
    uint Layer : SV_RenderTargetArrayIndex;
};

[maxvertexcount(3)]
void GS(triangle VS_OUTPUT input[3], inout TriangleStream<GS_OUTPUT> stream)
{
    for (uint i = 0; i < 3; ++i)
    {
        GS_OUTPUT output;
        output.Position = input[i].Position;
        output.TexC = input[i].TexC;
        output.MaterialIndex = input[i].MaterialIndex;
        output.Layer = input[i].Layer;
        stream.Append(output);
    }
}
#endif

// Keep the VS/GS field order for shader linkage, including the unused SV_POSITION.
struct PS_INPUT
{
    float4 Position : SV_POSITION;
    float2 TexC : TEXCOORD0;
    nointerpolation uint MaterialIndex : TEXCOORD1;
};

void PS(PS_INPUT input)
{
    Material material = MaterialCache[input.MaterialIndex];
    if (material.RenderLayer == RENDER_LAYER_MASKED)
        clip(Texture2DCache[material.DiffuseMapIndex].Sample(samAnisotropicWrap, input.TexC).a - 0.5f);
}
