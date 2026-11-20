#include "CBufferStructures.hlsl"
cbuffer LindenView : register(b0)
{
    float4x4 ViewProjection;
    uint MaterialIndex, FirstIndex;
};
ConstantBuffer<CameraCB> CBCamera : register(b1);
ConstantBuffer<MainCB> CBMain : register(b2);
ConstantBuffer<ShadowCB> CBShadow : register(b3);
struct PoseVertex { float3 Position, Normal; };
StructuredBuffer<PoseVertex> Pose : register(t0);
StructuredBuffer<Material> MaterialCache : register(t1);
Texture2D<float> ShadowAtlas : register(t2);
TextureCube<float4> IrradianceEnvironment : register(t3);
TextureCube<float4> PrefilteredEnvironment : register(t4);
Texture2D<float4> EnvironmentBRDF : register(t5);
StructuredBuffer<LocalLight> LocalLights : register(t6);
Texture2DArray<float> LocalShadowMaps : register(t7);
StructuredBuffer<uint> Indices : register(t8);
StructuredBuffer<float2> UVs : register(t9);
StructuredBuffer<PoseVertex> PreviousPose : register(t10);
Texture2D Texture2DCache[TEXTURE2D_RANGE_LENGTH] : register(t11);
#include "VegetationSurface.hlsli"

DepthOutput DepthVS(uint corner : SV_VertexID)
{
    uint i = Indices[FirstIndex + corner];
    DepthOutput output;
    output.Position = mul(float4(Pose[i].Position, 1), ViewProjection);
    output.UV = UVs[i];
    return output;
}

VertexOutput VS(uint corner : SV_VertexID)
{
    uint i = Indices[FirstIndex + corner];
    PoseVertex vertex = Pose[i];
    VertexOutput output = (VertexOutput)0;
    output.Position = mul(float4(vertex.Position, 1), ViewProjection);
    output.WorldPosition = vertex.Position;
    output.Normal = vertex.Normal;
    output.UV = UVs[i];
#ifndef VEGETATION_CAPTURE
    output.CurrentClip = mul(float4(vertex.Position, 1), CBCamera.ViewProjNoJitter);
    output.PreviousClip = mul(float4(PreviousPose[i].Position, 1), CBCamera.PrevViewProjNoJitter);
#endif
    return output;
}
