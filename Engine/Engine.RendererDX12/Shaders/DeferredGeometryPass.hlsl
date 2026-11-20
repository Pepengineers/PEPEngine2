#include "CBufferStructures.hlsli"
#include "PBRMath.hlsli"

ConstantBuffer<IndirectConstants> CBIndirectConstants : register(b0);
ConstantBuffer<MainCB> CBMain : register(b1);
ConstantBuffer<CameraCB> CBCamera : register(b2);

SamplerState samAnisotropicWrap : register(s4);

StructuredBuffer<Material> MaterialCache : register(t0);
StructuredBuffer<Transform> TransformCache : register(t1);
StructuredBuffer<InstanceData> InstanceCache : register(t2);
Texture2D Texture2DCache[TEXTURE2D_RANGE_LENGTH] : register(t3);

struct VS_INPUT
{
    float3 Pos : POSITION;
    float2 TexC : TEXCOORD;
    float3 Normal : NORMAL;
};

struct VS_OUTPUT_PS_INPUT
{
    precise float4 PosCS : SV_POSITION;
    float4 PrevPosCSNoJitter : TEXCOORD2;
    float3 PosW : POSITION;
    float4 PosCSNoJitter : POSITION2;
    float2 TexC : TEXCOORD;
    float3 Normal : NORMAL;
    nointerpolation uint MaterialIndex : TEXCOORD1;
};

VS_OUTPUT_PS_INPUT VS(VS_INPUT vin)
{
    VS_OUTPUT_PS_INPUT vout = (VS_OUTPUT_PS_INPUT) 0.0f;
	
    InstanceData instance = InstanceCache[CBIndirectConstants.InstanceID];
    Transform transform = TransformCache[instance.TransformIndex];
    
    float4 posW = mul(float4(vin.Pos, 1.0f), transform.World);
    vout.PosW = posW.xyz;

    vout.Normal = TransformNormal(vin.Normal, (float3x3)transform.World);
    vout.PosCS = mul(posW, CBCamera.ViewProj);
    
    vout.PosCSNoJitter = mul(posW, CBCamera.ViewProjNoJitter);
    float4 prevPosW = mul(float4(vin.Pos, 1.0f), transform.PrevWorld);
    vout.PrevPosCSNoJitter = mul(prevPosW, CBCamera.PrevViewProjNoJitter);
    
    vout.TexC = vin.TexC;
    vout.MaterialIndex = instance.MaterialIndex;
    
    return vout;
}

float4 SampleBaseColor(Material material, float2 uv)
{
    float4 color = Texture2DCache[material.DiffuseMapIndex].Sample(samAnisotropicWrap, uv);
    if (material.RenderLayer == RENDER_LAYER_MASKED) clip(color.a - 0.5f);
    return color;
}

void DepthPS(VS_OUTPUT_PS_INPUT pin)
{
    Material material = MaterialCache[pin.MaterialIndex];
    if (material.RenderLayer == RENDER_LAYER_MASKED)
        SampleBaseColor(material, pin.TexC);
}

// Transforms normal by a NormalMap using position and UV derivatives
// Falls back to the mesh normal if the map is missing
float3 SurfaceNormal(Material material, float3 normal, float3 position, float2 uv)
{
    float3 n = SafeNormalize(normal);
    if (material.NormalMapIndex != NO_TEXTURE)
    {
        float3 dpdx = ddx(position), dpdy = ddy(position);
        float2 duvdx = ddx(uv), duvdy = ddy(uv);
        float3 dx = cross(dpdy, n), dy = cross(n, dpdx);
        float3 tangent = dx * duvdx.x + dy * duvdy.x;
        float3 bitangent = dx * duvdx.y + dy * duvdy.y;
        float lengthSq = max(dot(tangent, tangent), dot(bitangent, bitangent));
        if (lengthSq >= 1e-12f)
        {
            float3 mapped = Texture2DCache[material.NormalMapIndex].SampleGrad(samAnisotropicWrap, uv, duvdx, duvdy).xyz * 2 - 1;
            n = SafeNormalize((mapped.x * tangent + mapped.y * bitangent) * rsqrt(lengthSq) + mapped.z * n);
        }
    }
    return n;
}

void SampleMaterial(Material material, float3 albedo, float3 normal, float3 position, float2 uv,
    out PBRSurface surface, out float3 emissive)
{
    float roughness = material.Roughness;
    if (material.RoughnessMapIndex != NO_TEXTURE)
        roughness *= Texture2DCache[material.RoughnessMapIndex].Sample(samAnisotropicWrap, uv).r;
    float3 specular = saturate(material.SpecularColor);
    if (material.SpecularMapIndex != NO_TEXTURE)
        specular *= Texture2DCache[material.SpecularMapIndex].Sample(samAnisotropicWrap, uv).rgb;
    emissive = material.EmissiveColor;
    if (material.EmissiveMapIndex != NO_TEXTURE)
        emissive *= Texture2DCache[material.EmissiveMapIndex].Sample(samAnisotropicWrap, uv).rgb;

    surface.Albedo = albedo;
    surface.Metallic = saturate(material.Metallic);
    surface.F0 = lerp(specular, albedo, surface.Metallic);
    surface.Roughness = clamp(roughness, 0.045f, 1.0f);
    surface.Normal = SurfaceNormal(material, normal, position, uv);
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
    float4 color = SampleBaseColor(material, pin.TexC);
    PBRSurface surface;
    float3 emissive;
    SampleMaterial(material, color.rgb, pin.Normal, pin.PosW, pin.TexC, surface, emissive);
    output.AlbedoMetallic = float4(surface.Albedo, surface.Metallic);
    output.Velocity = pin.PrevPosCSNoJitter.w > 1e-5f
        ? pin.PrevPosCSNoJitter.xy / pin.PrevPosCSNoJitter.w - pin.PosCSNoJitter.xy / pin.PosCSNoJitter.w : 0;
    output.NormalRoughness = float4(surface.Normal, surface.Roughness);
    output.F0Baked = float4(surface.F0, material.UseBakedLighting);
    output.Emissive = float4(emissive, 0);
    
    return output;
}
