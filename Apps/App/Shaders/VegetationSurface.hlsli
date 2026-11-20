SamplerState samLinearClamp : register(s3);
SamplerState samAnisotropicWrap : register(s4);
SamplerComparisonState samShadow : register(s6);
#include "PBRSurface.hlsli"
#ifdef VEGETATION_CAPTURE
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
struct DepthOutput
{
    float4 Position : SV_POSITION;
    float2 UV : TEXCOORD0;
};
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
#ifdef VEGETATION_CAPTURE
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
