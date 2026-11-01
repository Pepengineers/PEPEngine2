#include "CBufferStructures.hlsl"

#ifdef PROBE_CULLING
#include "ShadowFrustum.hlsli"
cbuffer CullParameters : register(b0) { uint Geometry; uint Shadow; };
ConstantBuffer<CameraCB> CBCamera : register(b1);
ConstantBuffer<ShadowCB> CBShadow : register(b2);
StructuredBuffer<InstanceData> Instances : register(t0);
StructuredBuffer<IndirectDrawArgs> InputCommands : register(t1);
StructuredBuffer<Material> Materials : register(t2);
RWStructuredBuffer<IndirectDrawArgs> VisibleCommands : register(u0);
RWStructuredBuffer<uint> Counter : register(u1);

[numthreads(1, 1, 1)]
void ClearCS() { Counter[0] = 0; }

[numthreads(64, 1, 1)]
void CullCS(uint3 id : SV_DispatchThreadID)
{
    uint count, stride;
    InputCommands.GetDimensions(count, stride);
    if (id.x >= count) return;
    IndirectDrawArgs command = InputCommands[id.x];
    if (command.InstanceCount == 0) return;
    InstanceData instance = Instances[command.InstanceID];
    // shortcut: transparent surfaces are excluded, add transparency capture when glass must appear inside probes.
    if (Materials[instance.MaterialIndex].RenderLayer == 2) return;
    if (Geometry == 1 && instance.ReflectionDynamic != 0) return;
    if (Geometry == 2 && instance.ReflectionDynamic == 0) return;
    float4x4 matrix = Shadow != 0 ? CBShadow.ViewProj[0] : CBCamera.ViewProj;
    if (!ShadowBoxVisible(instance.BoundingBoxCenter, instance.BoundingBoxExtents, matrix)) return;
    uint index;
    InterlockedAdd(Counter[0], 1, index);
    VisibleCommands[index] = command;
}
#else
cbuffer DrawConstants : register(b0) { uint InstanceID; };
ConstantBuffer<MainCB> CBMain : register(b1);
ConstantBuffer<CameraCB> CBCamera : register(b2);
ConstantBuffer<ShadowCB> CBShadow : register(b3);
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
#define SKY_CB_REGISTER b4
#define SKY_TRANSMITTANCE_REGISTER t10
#include "SkyRendering.hlsli"
Texture2D Texture2DCache[TEXTURE2D_RANGE_LENGTH] : register(t11);
SamplerState samLinearClamp : register(s3);
SamplerState samAnisotropicWrap : register(s4);
SamplerComparisonState samShadow : register(s6);

#define PROBE_CAPTURE 1
float SunVisibility(float3 position, float3 normal, float nDotL)
{
    if ((CBMain.SunShadowFlags & 1u) == 0) return 1;
    float4 projected = mul(float4(position + normal * CBShadow.TexelWorldSize.x * (1 - nDotL), 1), CBShadow.ViewProj[0]);
    float3 shadow = projected.xyz / projected.w;
    float2 uv = shadow.xy * float2(0.5f, -0.5f) + 0.5f;
    if (any(uv < 0) || any(uv > 1) || shadow.z < 0 || shadow.z > 1) return 1;
    float visibility = 0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
            visibility += ShadowAtlas.SampleCmpLevelZero(samShadow, uv + float2(x, y) / 1024.0f, shadow.z - 0.00001f);
    return visibility / 9;
}
#include "PBR.hlsli"

struct VertexOutput
{
    float4 Position : SV_POSITION;
    float3 WorldPosition : POSITION;
    float3 Normal : NORMAL;
    float2 UV : TEXCOORD0;
    nointerpolation uint MaterialIndex : TEXCOORD1;
};
VertexOutput VS(float3 position : POSITION, float3 normal : NORMAL, float2 uv : TEXCOORD)
{
    InstanceData instance = InstanceCache[InstanceID];
    float4x4 world = TransformCache[instance.TransformIndex].World;
    VertexOutput output;
    float4 worldPosition = mul(float4(position, 1), world);
    output.Position = mul(worldPosition, CBCamera.ViewProj);
    output.WorldPosition = worldPosition.xyz;
    output.Normal = TransformNormal(normal, (float3x3)world);
    output.UV = uv;
    output.MaterialIndex = instance.MaterialIndex;
    return output;
}
float4 PS(VertexOutput input) : SV_TARGET
{
    Material material = MaterialCache[input.MaterialIndex];
    float4 color = Texture2DCache[material.DiffuseIndex].Sample(samAnisotropicWrap, input.UV);
    if (material.RenderLayer == 1) clip(color.a - 0.5f);
    return float4(ShadePBR(material, color.rgb, input.WorldPosition, input.Normal, input.UV), 1);
}
VertexOutput ShadowVS(float3 position : POSITION, float3 normal : NORMAL, float2 uv : TEXCOORD)
{
    VertexOutput output = VS(position, normal, uv);
    output.Position = mul(float4(output.WorldPosition, 1), CBShadow.ViewProj[0]);
    return output;
}
void ShadowPS(VertexOutput input)
{
    Material material = MaterialCache[input.MaterialIndex];
    if (material.RenderLayer == 1)
        clip(Texture2DCache[material.DiffuseIndex].Sample(samAnisotropicWrap, input.UV).a - 0.5f);
}
float4 SkyVS(uint vertex : SV_VertexID) : SV_POSITION
{
    float2 uv = float2((vertex << 1) & 2, vertex & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 SkyPS(float4 position : SV_POSITION) : SV_TARGET
{
    // Capture viewport size is supplied through the camera's otherwise unused padding.
    float2 ndc = position.xy / CBCamera._pad1 * float2(2, -2) + float2(-1, 1);
    float4 world = mul(float4(ndc, 0, 1), CBCamera.InvViewProj);
    float3 direction = world.xyz / world.w - CBCamera.CameraLocation;
    return float4(SkyEnvironment.SampleLevel(samLinearClamp, direction, 0).rgb, 1);
}
#endif
