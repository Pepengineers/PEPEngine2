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
SamplerState samLinearClamp : register(s3);
SamplerState samAnisotropicWrap : register(s4);
SamplerComparisonState samShadow : register(s6);
#include "PBRSurface.hlsli"
#ifdef GRASS_CAPTURE
#define PROBE_CAPTURE 1
#include "CSM.hlsli"
#include "PBR.hlsli"
#endif
struct VertexOutput
{
    float4 Position : SV_POSITION;
    float3 WorldPosition : POSITION;
    float3 Normal : NORMAL;
    float2 UV : TEXCOORD0;
    float4 CurrentClip : TEXCOORD1;
    float4 PreviousClip : TEXCOORD2;
};
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
struct DepthOutput
{
    float4 Position : SV_POSITION;
    float2 UV : TEXCOORD0;
};
DepthOutput DepthVS(uint id : SV_VertexID)
{
    GrassVertex vertex = Vertices[id];
    DepthOutput output;
    output.Position = mul(float4(vertex.Position, 1), ViewProjection);
    output.UV = vertex.UV;
    return output;
}
void AlphaTest(float2 uv)
{
    Material material = MaterialCache[MaterialIndex];
    if (material.RenderLayer == 1)
        clip(Texture2DCache[material.DiffuseIndex].Sample(samAnisotropicWrap, uv).a - 0.5f);
}
void DepthPS(DepthOutput input) { AlphaTest(input.UV); }
PBRSurface Surface(VertexOutput input, bool front)
{
    Material material = MaterialCache[MaterialIndex];
    float4 color = Texture2DCache[material.DiffuseIndex].Sample(samAnisotropicWrap, input.UV);
    if (material.RenderLayer == 1) clip(color.a - 0.5f);
    return SamplePBRSurface(material, color.rgb, input.WorldPosition, front ? input.Normal : -input.Normal, input.UV);
}
float4 PrepassPS(VertexOutput input, bool front : SV_IsFrontFace) : SV_TARGET0
{
    PBRSurface surface = Surface(input, front);
    return float4(surface.Normal, surface.Roughness);
}
struct GBufferOutput
{
    float4 AlbedoMetallic : SV_TARGET0;
    float2 Velocity : SV_TARGET1;
    float4 NormalRoughness : SV_TARGET2;
    float4 F0Baked : SV_TARGET3;
    float4 Emissive : SV_TARGET4;
};
GBufferOutput PS(VertexOutput input, bool front : SV_IsFrontFace)
{
    PBRSurface s = Surface(input, front);
    GBufferOutput output;
    output.AlbedoMetallic = float4(s.Albedo, s.Metallic);
    output.Velocity = input.PreviousClip.w > 1e-5f ? input.PreviousClip.xy / input.PreviousClip.w - input.CurrentClip.xy / input.CurrentClip.w : 0;
    output.NormalRoughness = float4(s.Normal, s.Roughness);
    output.F0Baked = float4(s.F0, s.Baked);
    output.Emissive = float4(s.Emissive, 0);
    return output;
}
#ifdef GRASS_CAPTURE
float4 CapturePS(VertexOutput input, bool front : SV_IsFrontFace) : SV_TARGET0
{
    PBRSurface s = Surface(input, front);
    float3 v = SafeNormalize(CBCamera.CameraLocation - input.WorldPosition);
    float3 result = ShadeSunAndIBL(s, input.WorldPosition, v);
    for (uint i = 0; i < CBMain.LocalLightCount; ++i)
        result += ShadeLocalLight(s, input.WorldPosition, v, LocalLights[i]);
    return float4(result, 1);
}
#endif
