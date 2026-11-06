#include "CBufferStructures.hlsl"
#include "InstancedGeometry.hlsli"

ConstantBuffer<IndirectConstants> CBIndirectConstants : register(b0);
ConstantBuffer<MainCB> CBMain : register(b1);
ConstantBuffer<CameraCB> CBCamera : register(b2);
ConstantBuffer<ShadowCB> CBShadow : register(b3);

SamplerState samPointWrap : register(s0);
SamplerState samPointClamp : register(s1);
SamplerState samLinearWrap : register(s2);
SamplerState samLinearClamp : register(s3);
SamplerState samAnisotropicWrap : register(s4);
SamplerState samAnisotropicClamp : register(s5);

StructuredBuffer<Material> MaterialCache : register(t0);
StructuredBuffer<Transform> TransformCache : register(t1);
StructuredBuffer<InstanceData> InstanceCache : register(t2);
Texture2D<float> ShadowAtlas : register(t3);
TextureCube<float4> IrradianceEnvironment : register(t4);
TextureCube<float4> PrefilteredEnvironment : register(t5);
Texture2D<float4> EnvironmentBRDF : register(t6);
StructuredBuffer<LocalLight> LocalLights : register(t7);
Texture2DArray<float> LocalShadowMaps : register(t8);
Texture2D<float> SeparatorDepth : register(t14);
Texture2D<float> SceneDepth : register(t15);
Texture2D Texture2DCache[TEXTURE2D_RANGE_LENGTH] : register(t16);
SamplerComparisonState samShadow : register(s6);

#define SKY_CB_REGISTER b4
#define SKY_TRANSMITTANCE_REGISTER t11
#define SKY_AERIAL_RADIANCE_REGISTER t12
#define SKY_AERIAL_TRANSMITTANCE_REGISTER t13
#include "SkyRendering.hlsli"
#define LOCAL_REFLECTION_PROBES 1
#include "ReflectionProbes.hlsli"
#include "PBR.hlsli"

struct VS_INPUT
{
    float3 Pos : POSITION;
    float2 TexC : TEXCOORD;
    float3 Normal : NORMAL;
    float3 Tangent : TANGENT;
};

struct VS_OUTPUT_PS_INPUT
{
    float4 PosCS : SV_POSITION;
    float3 PosW : POSITION;
    float2 TexC : TEXCOORD;
    float3 Normal : NORMAL;
    float3 Tangent : TANGENT;
    uint MaterialIndex : TEXCOORD1;
    float LinearDepth : TEXCOORD2;
};

VS_OUTPUT_PS_INPUT VS(VS_INPUT vin, uint vertexID : SV_VertexID, uint instanceID : SV_InstanceID)
{
    VS_OUTPUT_PS_INPUT vout = (VS_OUTPUT_PS_INPUT) 0.0f;
	
    InstanceData instance = InstanceCache[DrawInstance(CBIndirectConstants.InstanceID, instanceID, InstanceCache[CBIndirectConstants.InstanceID])];
    if (instance.SkinOutputVertex != 0xffffffffu)
    {
        SkinVertex vertex = ReadSkinVertex(instance, vertexID);
        vin.Pos = vertex.Position;
        vin.Normal = vertex.Normal;
        vin.TexC = vertex.UV;
        vin.Tangent = vertex.Tangent;
    }
    float4x4 World = TransformCache[instance.TransformIndex].World;
    
    float4 posW = mul(float4(vin.Pos, 1.0f), World);
    vout.PosW = posW.xyz;

    vout.Normal = TransformNormal(vin.Normal, (float3x3) World);
    vout.Tangent = normalize(mul(vin.Tangent, (float3x3) World));
    vout.PosCS = mul(posW, CBCamera.ViewProj);
    vout.TexC = vin.TexC;
    float3 viewPos = mul(posW, CBCamera.View).xyz;
    vout.LinearDepth = abs(viewPos.z);
    vout.MaterialIndex = instance.MaterialIndex;
    
    return vout;
}

struct PSOutput
{
    float4 AccumColor : SV_TARGET0;
    float Revealage : SV_TARGET1;
};

PSOutput ShadeTransparency(VS_OUTPUT_PS_INPUT pin, uint partition)
{
    PSOutput output;
    if (partition != 0)
    {
        uint2 pixel = (uint2)pin.PosCS.xy;
        float separator = SeparatorDepth[pixel];
        bool hasSeparator = separator > SceneDepth[pixel];
        bool near = hasSeparator && pin.PosCS.z > separator;
        if ((partition == 1 && near) || (partition == 2 && !near)) discard;
    }
    
    Material material = MaterialCache[pin.MaterialIndex];
    float4 color = Texture2DCache[material.DiffuseIndex].Sample(samAnisotropicWrap, pin.TexC);
    float alpha = saturate(color.a * material.Opacity);
    
    clip(alpha - 0.001f);
    
    float weight = max(0.01f, alpha);
    float3 litColor = ShadePBR(material, color.rgb, pin.PosW, pin.Normal, pin.TexC);
    litColor = ApplySkyAerialPerspective(litColor, pin.PosCS.xy / CBSky.Box.zw,
        length(pin.PosW - CBCamera.CameraLocation));
    output.AccumColor = float4(litColor * alpha, alpha) * weight;
    output.Revealage = alpha;
    
    return output;
}

PSOutput PS(VS_OUTPUT_PS_INPUT pin) { return ShadeTransparency(pin, 0); }
PSOutput PSFar(VS_OUTPUT_PS_INPUT pin) { return ShadeTransparency(pin, 1); }
PSOutput PSNear(VS_OUTPUT_PS_INPUT pin) { return ShadeTransparency(pin, 2); }
