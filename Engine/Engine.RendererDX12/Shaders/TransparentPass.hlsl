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

VS_OUTPUT_PS_INPUT VS(VS_INPUT vin)
{
    VS_OUTPUT_PS_INPUT vout = (VS_OUTPUT_PS_INPUT) 0.0f;
	
    InstanceData instance = InstanceCache[CBIndirectConstants.InstanceID];
    float4x4 World = TransformCache[instance.TransformIndex].World;
    
    float4 posW = mul(float4(vin.Pos, 1.0f), World);
    vout.PosW = posW.xyz;

    // Assumes nonuniform scaling; otherwise, need to use inverse-transpose of world matrix.
    vout.Normal = normalize(mul(vin.Normal, (float3x3) World));
    vout.Tangent = normalize(mul(vin.Tangent, (float3x3) World));
    vout.PosCS = mul(posW, CBCamera.ViewProj);
    vout.TexC = vin.TexC;
    float3 viewPos = mul(posW, CBCamera.View).xyz;
    vout.LinearDepth = abs(viewPos.z);
    vout.MaterialIndex = instance.MaterialIndex;
    
    return vout;
}

float ComputeWeight(float alpha, float linearDepth, float farPlane)
{
    float power = 2.0f;
    float weight = alpha * (1.0 - alpha);
    float normalizedDepth = saturate(linearDepth / farPlane);
    float depthFactor = pow(1.0 - normalizedDepth, power);
    return weight * depthFactor;
}

struct PSOutput
{
    float4 AccumColor : SV_TARGET0;
    float Revealage : SV_TARGET1;
};

PSOutput PS(VS_OUTPUT_PS_INPUT pin)
{
    PSOutput output;
    
    Material material = MaterialCache[pin.MaterialIndex];
    float4 color = Texture2DCache[material.DiffuseIndex].Sample(samAnisotropicWrap, pin.TexC);
    
    float weight = ComputeWeight(color.a, pin.LinearDepth, CBCamera.FarPlane);
    
    output.AccumColor = float4(color.rgb * weight, weight);
    output.Revealage = weight;
    
    return output;
}