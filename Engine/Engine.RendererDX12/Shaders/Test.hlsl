#include "CBufferStructures.hlsl"

ConstantBuffer<MainCB> CBMain : register(b0);
ConstantBuffer<CameraCB> CBCamera : register(b1);

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
};

VS_OUTPUT_PS_INPUT VS(VS_INPUT vin, uint instanceID : SV_StartInstanceLocation)
{
    VS_OUTPUT_PS_INPUT vout = (VS_OUTPUT_PS_INPUT) 0.0f;
	
    InstanceData instance = InstanceCache[instanceID];
    float4x4 World = TransformCache[instance.TransformIndex].World;
    
    float4 posW = mul(float4(vin.Pos, 1.0f), World);
    vout.PosW = posW.xyz;

    // Assumes nonuniform scaling; otherwise, need to use inverse-transpose of world matrix.
    vout.Normal = normalize(mul(vin.Normal, (float3x3) World));
    vout.Tangent = normalize(mul(vin.Tangent, (float3x3) World));
    vout.PosCS = mul(posW, CBCamera.ViewProj);
    vout.TexC = vin.TexC;
    
    vout.MaterialIndex = instance.MaterialIndex;
    
    return vout;
}

float4 PS(VS_OUTPUT_PS_INPUT pin) : SV_Target
{
    Material material = MaterialCache[pin.MaterialIndex];
    
    return Texture2DCache[material.DiffuseIndex].Sample(samAnisotropicWrap, pin.TexC);
}