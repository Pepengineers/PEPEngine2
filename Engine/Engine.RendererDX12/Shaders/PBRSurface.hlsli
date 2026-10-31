#ifndef PBR_SURFACE_HLSLI
#define PBR_SURFACE_HLSLI
float3 SafeNormalize(float3 value) { return value * rsqrt(max(dot(value, value), 1e-12f)); }
struct PBRSurface
{
    float3 Albedo, Normal, F0, Emissive;
    float Roughness, Metallic;
    uint Baked;
};
#ifndef PBR_LIGHTING_ONLY
float3 TransformNormal(float3 normal, float3x3 world)
{
    float3 c0 = cross(world[1], world[2]), c1 = cross(world[2], world[0]), c2 = cross(world[0], world[1]);
    float sign = dot(world[0], c0) < 0 ? -1.0f : 1.0f;
    return SafeNormalize(mul(normal, float3x3(c0, c1, c2)) * sign);
}
float3 SampleSurfaceNormal(Material material, float3 normal, float3 position, float2 uv)
{
    float3 n = SafeNormalize(normal);
    if ((material.Flags & 1u) == 0u) return n;
    float3 normalTS = Texture2DCache[material.NormalIndex].Sample(samAnisotropicWrap, uv).xyz * 2 - 1;
    float3 dpdx = ddx(position), dpdy = ddy(position);
    float2 duvdx = ddx(uv), duvdy = ddy(uv);
    float3 dpdyPerp = cross(dpdy, n), dpdxPerp = cross(n, dpdx);
    float3 tangent = dpdyPerp * duvdx.x + dpdxPerp * duvdy.x;
    float3 bitangent = dpdyPerp * duvdx.y + dpdxPerp * duvdy.y;
    float frameLength = max(dot(tangent, tangent), dot(bitangent, bitangent));
    if (frameLength < 1e-12f) return n;
    float scale = rsqrt(frameLength);
    return SafeNormalize(normalTS.x * tangent * scale + normalTS.y * bitangent * scale + normalTS.z * n);
}
PBRSurface SamplePBRSurface(Material material, float3 albedo, float3 position, float3 normal, float2 uv)
{
    PBRSurface s;
    s.Albedo = albedo;
    s.Normal = SampleSurfaceNormal(material, normal, position, uv);
    s.Roughness = material.Roughness;
    if ((material.Flags & 4u) != 0u) s.Roughness *= Texture2DCache[material.RoughnessIndex].Sample(samAnisotropicWrap, uv).r;
    s.Roughness = clamp(s.Roughness, 0.045f, 1.0f);
    s.Metallic = saturate(material.Metallic);
    float3 specularColor = saturate(material.SpecularColor);
    if ((material.Flags & 2u) != 0u) specularColor *= Texture2DCache[material.SpecularIndex].Sample(samAnisotropicWrap, uv).rgb;
    s.F0 = lerp(specularColor, albedo, s.Metallic);
    s.Emissive = material.EmissiveColor;
    if ((material.Flags & 8u) != 0u) s.Emissive *= Texture2DCache[material.EmissiveIndex].Sample(samAnisotropicWrap, uv).rgb;
    s.Baked = (material.Flags & 16u) != 0u;
    return s;
}
#endif
#endif
