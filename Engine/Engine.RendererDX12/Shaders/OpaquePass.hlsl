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

    // Assumes nonuniform scaling; otherwise, need to use inverse-transpose of world matrix.
    vout.Normal = normalize(mul(vin.Normal, (float3x3) transform.World));
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
    float4 Color : SV_TARGET0;
    float2 Velocity : SV_TARGET1;
};

PS_OUTPUT PS(VS_OUTPUT_PS_INPUT pin)
{
    PS_OUTPUT output;
    
    Material material = MaterialCache[pin.MaterialIndex];
    float4 color = Texture2DCache[material.DiffuseIndex].Sample(samAnisotropicWrap, pin.TexC);
    
    float2 currentNDC = pin.PosCSNoJitter.xy / pin.PosCSNoJitter.w;
    float2 prevNDC = pin.PrevPosCSNoJitter.xy / pin.PrevPosCSNoJitter.w;
    float2 velocity = prevNDC - currentNDC;

    output.Color = float4(color.rgb, 1.0f);
    output.Velocity = velocity;
    
    return output;
}