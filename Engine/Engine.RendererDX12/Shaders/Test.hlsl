#include "CBufferStructures.hlsl"

ConstantBuffer<MainCB> CBMain : register(b0);
ConstantBuffer<CameraCB> CBCamera : register(b1);
ConstantBuffer<TransformCB> CBTransform : register(b2);
ConstantBuffer<MaterialCB> CBMaterial : register(b3);

SamplerState samPointWrap : register(s0);
SamplerState samPointClamp : register(s1);
SamplerState samLinearWrap : register(s2);
SamplerState samLinearClamp : register(s3);
SamplerState samAnisotropicWrap : register(s4);
SamplerState samAnisotropicClamp : register(s5);

Texture2D DiffuseMap : register(t0);
Texture2D NormalMap : register(t1);
Texture2D DisplacementMap : register(t2);

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
};


VS_OUTPUT_PS_INPUT VS(VS_INPUT vin)
{
    VS_OUTPUT_PS_INPUT vout = (VS_OUTPUT_PS_INPUT) 0.0f;
	
    float4 posW = mul(float4(vin.Pos, 1.0f), CBTransform.World);
    vout.PosW = posW.xyz;

    // Assumes nonuniform scaling; otherwise, need to use inverse-transpose of world matrix.
    vout.Normal = normalize(mul(vin.Normal, (float3x3) CBTransform.World));
    vout.Tangent = normalize(mul(vin.Tangent, (float3x3) CBTransform.World));
    vout.PosCS = mul(posW, CBCamera.ViewProj);
    vout.TexC = vin.TexC;
    
    return vout;
}

float4 PS(VS_OUTPUT_PS_INPUT pin) : SV_Target
{
    return DiffuseMap.Sample(samAnisotropicWrap, pin.TexC);
}