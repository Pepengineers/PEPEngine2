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
Texture2D SpecularMap : register(t2);
Texture2D RoughnessMap : register(t3);
Texture2D EmissiveMap : register(t4);

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
    float4 diffuseSample = DiffuseMap.Sample(samAnisotropicWrap, pin.TexC);
    if (CBMaterial.UseBakedLighting > 0.5f)
    {
        return diffuseSample;
    }

    float3 normalW = normalize(pin.Normal);
    if (CBMaterial.HasNormalMap > 0.5f)
    {
        float3 tangentW = normalize(pin.Tangent - normalW * dot(pin.Tangent, normalW));
        float3 bitangentW = normalize(cross(normalW, tangentW));
        float3 normalT = NormalMap.Sample(samLinearWrap, pin.TexC).xyz * 2.0f - 1.0f;
        normalW = normalize(normalT.x * tangentW + normalT.y * bitangentW + normalT.z * normalW);
    }

    float roughness = saturate(CBMaterial.Roughness);
    if (CBMaterial.HasRoughnessMap > 0.5f)
    {
        roughness = saturate(RoughnessMap.Sample(samLinearWrap, pin.TexC).r);
    }
    roughness = max(roughness, 0.04f);

    float3 specularColor = CBMaterial.SpecularColor;
    if (CBMaterial.HasSpecularMap > 0.5f)
    {
        specularColor = SpecularMap.Sample(samLinearWrap, pin.TexC).rgb;
    }

    float3 emissive = CBMaterial.EmissiveColor;
    if (CBMaterial.HasEmissiveMap > 0.5f)
    {
        emissive = EmissiveMap.Sample(samLinearWrap, pin.TexC).rgb * CBMaterial.EmissiveColor;
    }

    float3 lightDirectionW = normalize(float3(-0.35f, 0.85f, -0.45f));
    float3 viewDirectionW = normalize(CBCamera.CameraLocation - pin.PosW);
    float3 halfVectorW = normalize(lightDirectionW + viewDirectionW);

    float ndotl = saturate(dot(normalW, lightDirectionW));
    float shininess = max(2.0f / (roughness * roughness) - 2.0f, 1.0f);
    float specularPower = pow(saturate(dot(normalW, halfVectorW)), shininess);

    float3 ambient = diffuseSample.rgb * 0.22f;
    float3 diffuse = diffuseSample.rgb * ndotl;
    float3 specular = specularColor * specularPower * (1.0f - roughness) * ndotl;

    return float4(ambient + diffuse + specular + emissive, diffuseSample.a);
}