#include "CBufferStructures.hlsl"

ConstantBuffer<IndirectConstants> CBIndirectConstants : register(b0);
ConstantBuffer<MainCB> CBMain : register(b1);
ConstantBuffer<CameraCB> CBCamera : register(b2);

SamplerState samPointWrap : register(s0);
SamplerState samPointClamp : register(s1);
SamplerState samLinearWrap : register(s2);
SamplerState samLinearClamp : register(s3);
SamplerState samAnisotropicWrap : register(s4);
SamplerState samAnisotropicClamp : register(s5);

StructuredBuffer<Material> MaterialCache : register(t0);
StructuredBuffer<Transform> TransformCache : register(t1);
StructuredBuffer<InstanceData> InstanceCache : register(t2);
Texture2D Texture2DCache[TEXTURE2D_RANGE_LENGTH] : register(t3);

#include "PBRSurface.hlsli"

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
    float4 PrevPosCSNoJitter : TEXCOORD2;
    float3 PosW : POSITION;
    float4 PosCSNoJitter : POSITION2;
    float2 TexC : TEXCOORD;
    float3 Normal : NORMAL;
    float3 Tangent : TANGENT;
    uint MaterialIndex : TEXCOORD1;
};

VS_OUTPUT_PS_INPUT VS(VS_INPUT vin)
{
    VS_OUTPUT_PS_INPUT vout = (VS_OUTPUT_PS_INPUT) 0.0f;
	
    InstanceData instance = InstanceCache[CBIndirectConstants.InstanceID];
    Transform transform = TransformCache[instance.TransformIndex];
    
    float4 posW = mul(float4(vin.Pos, 1.0f), transform.World);
    vout.PosW = posW.xyz;

    vout.Normal = TransformNormal(vin.Normal, (float3x3) transform.World);
    vout.Tangent = normalize(mul(vin.Tangent, (float3x3) transform.World));
    vout.PosCS = mul(posW, CBCamera.ViewProj);
    
    vout.PosCSNoJitter = mul(posW, CBCamera.ViewProjNoJitter);
    float4 prevPosW = mul(float4(vin.Pos, 1.0f), transform.PrevWorld);
    vout.PrevPosCSNoJitter = mul(prevPosW, CBCamera.PrevViewProjNoJitter);
    
    vout.TexC = vin.TexC;
    vout.MaterialIndex = instance.MaterialIndex;
    
    return vout;
}

struct PS_OUTPUT
{
    float4 AlbedoMetallic : SV_TARGET0;
    float2 Velocity : SV_TARGET1;
    float4 NormalRoughness : SV_TARGET2;
    float4 F0Baked : SV_TARGET3;
    float4 Emissive : SV_TARGET4;
};

PS_OUTPUT PS(VS_OUTPUT_PS_INPUT pin)
{
    PS_OUTPUT output;
    
    Material material = MaterialCache[pin.MaterialIndex];
    float4 color = Texture2DCache[material.DiffuseIndex].Sample(samAnisotropicWrap, pin.TexC);
    if (material.RenderLayer == 1) clip(color.a - 0.5f);
    
    float2 currentNDC = pin.PosCSNoJitter.xy / pin.PosCSNoJitter.w;
    float2 prevNDC = pin.PrevPosCSNoJitter.xy / pin.PrevPosCSNoJitter.w;
    float2 velocity = prevNDC - currentNDC;

    PBRSurface surface = SamplePBRSurface(material, color.rgb, pin.PosW, pin.Normal, pin.TexC);
    output.AlbedoMetallic = float4(surface.Albedo, surface.Metallic);
    output.NormalRoughness = float4(surface.Normal, surface.Roughness);
    output.F0Baked = float4(surface.F0, surface.Baked);
    output.Emissive = float4(surface.Emissive, 0.0f);
    output.Velocity = velocity;
    
    return output;
}
