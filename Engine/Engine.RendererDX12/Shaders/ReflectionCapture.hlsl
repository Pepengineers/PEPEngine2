#include "CBufferStructures.hlsl"
ConstantBuffer<IndirectConstants> CBIndirectConstants : register(b0);
ConstantBuffer<MainCB> CBMain : register(b1);
ConstantBuffer<CameraCB> CBCamera : register(b2);
cbuffer ProbeShadow : register(b3)
{
    float4x4 ProbeShadowViewProjection;
    float ProbeShadowTexelWorld;
    uint CaptureSize;
    float2 Padding;
};
StructuredBuffer<Material> MaterialCache : register(t0);
StructuredBuffer<Transform> TransformCache : register(t1);
StructuredBuffer<InstanceData> InstanceCache : register(t2);
Texture2D<float> ShadowAtlas : register(t3);
TextureCube<float4> IrradianceEnvironment : register(t4);
TextureCube<float4> PrefilteredEnvironment : register(t5);
Texture2D<float4> EnvironmentBRDF : register(t6);
StructuredBuffer<LocalLight> LocalLights : register(t7);
Texture2DArray<float> LocalShadowMaps : register(t8);
TextureCube<float4> SkyEnvironment : register(t9);
Texture2D Texture2DCache[TEXTURE2D_RANGE_LENGTH] : register(t10);
SamplerState samLinearClamp : register(s3);
SamplerState samAnisotropicWrap : register(s4);
SamplerComparisonState samShadow : register(s6);

float SunVisibility(float3 position, float3 normal, float nDotL)
{
    if ((CBMain.SunShadowFlags & 1u) == 0) return 1;
    float4 clip = mul(float4(position + normal * ProbeShadowTexelWorld * (0.25 + 0.5 * (1-nDotL)), 1), ProbeShadowViewProjection);
    float3 projected = clip.xyz / clip.w;
    float2 uv = projected.xy * float2(0.5,-0.5)+0.5;
    if (any(uv < 0) || any(uv > 1) || projected.z < 0 || projected.z > 1) return 1;
    uint width, height; ShadowAtlas.GetDimensions(width,height);
    float visibility = 0;
    for (int y=-1;y<=1;++y) for (int x=-1;x<=1;++x)
        visibility += ShadowAtlas.SampleCmpLevelZero(samShadow, clamp(uv + float2(x,y)/float2(width,height),
            0.5/float2(width,height), 1-0.5/float2(width,height)), projected.z-0.00001);
    return visibility/9;
}
#define CSM_HLSLI
#include "PBR.hlsli"

struct CaptureVertex
{
    float3 Position : POSITION;
    float2 UV : TEXCOORD;
    float3 Normal : NORMAL;
    float3 Tangent : TANGENT;
};
struct CaptureVarying
{
    float4 Position : SV_POSITION;
    float3 World : POSITION;
    float3 Normal : NORMAL;
    float2 UV : TEXCOORD;
    nointerpolation uint MaterialIndex : TEXCOORD1;
};
CaptureVarying VS(CaptureVertex input)
{
    InstanceData instance = InstanceCache[CBIndirectConstants.InstanceID];
    Transform transform = TransformCache[instance.TransformIndex];
    CaptureVarying output;
    float4 world = mul(float4(input.Position,1),transform.World);
    output.World = world.xyz;
    output.Position = mul(world,CBCamera.ViewProj);
    output.Normal = TransformNormal(input.Normal,(float3x3)transform.World);
    output.UV = input.UV;
    output.MaterialIndex = instance.MaterialIndex;
    return output;
}
float4 PS(CaptureVarying input) : SV_TARGET
{
    Material material = MaterialCache[input.MaterialIndex];
    float4 color = Texture2DCache[material.DiffuseIndex].Sample(samAnisotropicWrap,input.UV);
    if (material.RenderLayer == 1) clip(color.a-0.5);
    return float4(ShadePBR(material,color.rgb,input.World,input.Normal,input.UV),1);
}
void ShadowPS(CaptureVarying input)
{
    Material material = MaterialCache[input.MaterialIndex];
    if (material.RenderLayer == 1) clip(Texture2DCache[material.DiffuseIndex].Sample(samAnisotropicWrap,input.UV).a-0.5);
}
float4 SkyVS(uint vertex : SV_VertexID) : SV_POSITION
{
    return float4(vertex==2?3:-1,vertex==1?3:-1,0,1);
}
float4 SkyPS(float4 position : SV_POSITION) : SV_TARGET
{
    float2 ndc = position.xy / CaptureSize * float2(2,-2)+float2(-1,1);
    float4 world = mul(float4(ndc,0.000001,1),CBCamera.InvViewProj);
    float3 ray = world.xyz - CBCamera.CameraLocation*world.w;
    return float4(SkyEnvironment.SampleLevel(samLinearClamp,normalize(ray),0).rgb,1);
}
