#ifndef PBR_HLSLI
#define PBR_HLSLI

#include "PBRMath.hlsli"

float3 SafeNormalize(float3 value)
{
    return value * rsqrt(max(dot(value, value), 1e-12f));
}

float3 TransformNormal(float3 normal, float3x3 world)
{
    float3 c0 = cross(world[1], world[2]);
    float3 c1 = cross(world[2], world[0]);
    float3 c2 = cross(world[0], world[1]);
    float determinantSign = dot(world[0], c0) < 0.0f ? -1.0f : 1.0f;
    return SafeNormalize(mul(normal, float3x3(c0, c1, c2)) * determinantSign);
}

float3 SampleSurfaceNormal(Material material, float3 normal, float3 position, float2 uv)
{
    float3 n = SafeNormalize(normal);
    if ((material.Flags & 1u) == 0u) return n;

    float3 normalTS = Texture2DCache[material.NormalIndex].Sample(samAnisotropicWrap, uv).xyz * 2.0f - 1.0f;
    float3 dpdx = ddx(position);
    float3 dpdy = ddy(position);
    float2 duvdx = ddx(uv);
    float2 duvdy = ddy(uv);
    float3 dpdyPerp = cross(dpdy, n);
    float3 dpdxPerp = cross(n, dpdx);
    float3 tangent = dpdyPerp * duvdx.x + dpdxPerp * duvdy.x;
    float3 bitangent = dpdyPerp * duvdx.y + dpdxPerp * duvdy.y;
    float frameLength = max(dot(tangent, tangent), dot(bitangent, bitangent));
    if (frameLength < 1e-12f) return n;
    float scale = rsqrt(frameLength);
    return SafeNormalize(normalTS.x * tangent * scale + normalTS.y * bitangent * scale + normalTS.z * n);
}

float3 ShadePBR(Material material, float3 albedo, float3 position, float3 normal, float2 uv)
{
    // Baked diffuse/emissive textures already include illumination.
    if ((material.Flags & 16u) != 0u) return albedo;

    float3 n = SampleSurfaceNormal(material, normal, position, uv);
    float3 v = SafeNormalize(CBCamera.CameraLocation - position);
    float3 l = SafeNormalize(-CBMain.LightDirection);
    float3 h = SafeNormalize(v + l);
    float nDotV = saturate(dot(n, v));
    float nDotL = saturate(dot(n, l));
    float nDotH = saturate(dot(n, h));
    float vDotH = saturate(dot(v, h));

    float roughness = material.Roughness;
    if ((material.Flags & 4u) != 0u)
        roughness *= Texture2DCache[material.RoughnessIndex].Sample(samAnisotropicWrap, uv).r;
    roughness = clamp(roughness, 0.045f, 1.0f);
    float metallic = saturate(material.Metallic);
    float3 specularColor = saturate(material.SpecularColor);
    if ((material.Flags & 2u) != 0u)
        specularColor *= Texture2DCache[material.SpecularIndex].Sample(samAnisotropicWrap, uv).rgb;
    float3 f0 = lerp(specularColor, albedo, metallic);
    float3 fresnel = f0 + (1.0f - f0) * FresnelSchlickFactor(vDotH);
    float3 diffuse = (1.0f - fresnel) * (1.0f - metallic) * albedo / PBR_PI;
    float3 specular = DistributionGGX(nDotH, roughness) * SmithGGXVisibility(nDotV, nDotL, roughness) * fresnel;
    float3 direct = (diffuse + specular) * CBMain.LightRadiance * nDotL;
    if (nDotV <= 0.0f) direct = 0.0f;

    // shortcut: constant diffuse ambient; replace with filtered environment maps when IBL is added.
    float3 ambient = CBMain.AmbientRadiance * albedo * (1.0f - metallic) * (1.0f - f0);
    float3 emissive = material.EmissiveColor;
    if ((material.Flags & 8u) != 0u)
        emissive *= Texture2DCache[material.EmissiveIndex].Sample(samAnisotropicWrap, uv).rgb;
    return direct + ambient + emissive;
}

#endif
