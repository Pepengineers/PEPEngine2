#include "CBufferStructures.hlsl"
#include "GrassCommon.hlsli"
ConstantBuffer<CameraCB> CBCamera : register(b1);
ConstantBuffer<MainCB> CBMain : register(b2);
ConstantBuffer<ShadowCB> CBShadow : register(b3);
StructuredBuffer<GrassVertex> Vertices : register(t0);
StructuredBuffer<Material> MaterialCache : register(t1);
Texture2D<float> ShadowAtlas : register(t2);
TextureCube<float4> IrradianceEnvironment : register(t3);
TextureCube<float4> PrefilteredEnvironment : register(t4);
Texture2D<float4> EnvironmentBRDF : register(t5);
StructuredBuffer<LocalLight> LocalLights : register(t6);
Texture2DArray<float> LocalShadowMaps : register(t7);
Texture2D Texture2DCache[TEXTURE2D_RANGE_LENGTH] : register(t8);
#ifdef GRASS_CAPTURE
#define VEGETATION_CAPTURE
#endif
#include "VegetationSurface.hlsli"
VertexOutput VS(uint id : SV_VertexID)
{
    GrassVertex vertex = Vertices[id];
    VertexOutput output;
    output.Position = mul(float4(vertex.Position, 1), ViewProjection);
    output.WorldPosition = vertex.Position;
    output.Normal = vertex.Normal;
    output.UV = vertex.UV;
    output.CurrentClip = mul(float4(vertex.Position, 1), CBCamera.ViewProjNoJitter);
    output.PreviousClip = mul(float4(vertex.PreviousPosition, 1), CBCamera.PrevViewProjNoJitter);
    return output;
}
DepthOutput DepthVS(uint id : SV_VertexID)
{
    GrassVertex vertex = Vertices[id];
    DepthOutput output;
    output.Position = mul(float4(vertex.Position, 1), ViewProjection);
    output.UV = vertex.UV;
    return output;
}
