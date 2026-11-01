#ifndef PBR_LIGHTING_HLSLI
#define PBR_LIGHTING_HLSLI
#include "PBRMath.hlsli"
#ifndef PROBE_CAPTURE
#include "CSM.hlsli"
#endif
#include "LocalShadows.hlsli"
float3 EvaluateBRDF(PBRSurface s, float3 v, float3 l)
{
    float nDotV = saturate(dot(s.Normal, v)), nDotL = saturate(dot(s.Normal, l));
    if (nDotV <= 0 || nDotL <= 0) return 0;
    float3 h = SafeNormalize(v + l);
    float3 fresnel = s.F0 + (1 - s.F0) * FresnelSchlickFactor(saturate(dot(v, h)));
    float3 diffuse = (1 - fresnel) * (1 - s.Metallic) * s.Albedo / PBR_PI;
    float3 specular = DistributionGGX(saturate(dot(s.Normal, h)), s.Roughness) * SmithGGXVisibility(nDotV, nDotL, s.Roughness) * fresnel;
    return (diffuse + specular) * nDotL;
}
float3 ShadeSunAndIBL(PBRSurface s, float3 position, float3 v)
{
    if (s.Baked != 0) return s.Albedo;
    float3 l = SafeNormalize(-CBMain.LightDirection);
    float3 direct = EvaluateBRDF(s, v, l) * CBMain.LightRadiance;
    if (any(direct > 0)) direct *= SunVisibility(position, s.Normal, saturate(dot(s.Normal, l)));
    float nDotV = saturate(dot(s.Normal, v));
    float3 irradiance = IrradianceEnvironment.SampleLevel(samLinearClamp, s.Normal, 0).rgb;
    float3 prefiltered = PrefilteredEnvironment.SampleLevel(samLinearClamp, reflect(-v, s.Normal), s.Roughness * 7).rgb;

#ifdef LOCAL_REFLECTION_PROBES
    prefiltered = SampleReflectionRadiance(position, reflect(-v, s.Normal), s.Roughness, prefiltered);
#endif
    float2 brdf = EnvironmentBRDF.SampleLevel(samLinearClamp, float2(nDotV, s.Roughness), 0).rg;
    float3 fresnel = s.F0 + (max(1 - s.Roughness, s.F0) - s.F0) * FresnelSchlickFactor(nDotV);
    return direct + (1 - fresnel) * (1 - s.Metallic) * irradiance * s.Albedo / PBR_PI + prefiltered * (s.F0 * brdf.x + brdf.y) + s.Emissive;
}
float3 ShadeLocalLight(PBRSurface s, float3 position, float3 v, LocalLight light)
{
    float3 delta = light.Position - position;
    float distanceSquared = dot(delta, delta);
    if (distanceSquared >= light.Range * light.Range || s.Baked != 0) return 0;
    float3 l = delta * rsqrt(max(distanceSquared, 1e-6f));
    float cone = light.Type == 2u ? saturate((dot(-l, light.Direction) - light.OuterCos) / max(light.InnerCos - light.OuterCos, 1e-4f)) : 1;
    float rangeFade = saturate(1 - pow(distanceSquared / (light.Range * light.Range), 2));
    // Bistro units are centimetres; intensity is expressed per square metre.
    float attenuation = rangeFade * rangeFade * cone * cone / max(distanceSquared * 0.0001f, 0.01f);
    float3 result = EvaluateBRDF(s, v, l) * light.Radiance * attenuation;
    if (any(result > 0)) result *= LocalVisibility(light, position, s.Normal, saturate(dot(s.Normal, l)));
    return result;
}
#endif
