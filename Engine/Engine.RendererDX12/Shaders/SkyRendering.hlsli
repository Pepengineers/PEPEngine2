#ifndef SKY_RENDERING_HLSLI
#define SKY_RENDERING_HLSLI
#include "SkyParameters.hlsli"
ConstantBuffer<SkyParameters> CBSky : register(SKY_CB_REGISTER);
Texture2D<float4> SkyTransmittance : register(SKY_TRANSMITTANCE_REGISTER);
SamplerState SkySampler : register(s7);

float3 SkySunTransmittance(float3 worldCm, float3 direction)
{
    if (CBSky.Mode.x != 2) return 1;
    float3 origin = SkyPlanetPosition(worldCm, CBSky);
    float radius = length(origin);
    if (radius < CBSky.CenterRadius.w) return 0;
    float2 ground = SkySphereRoots(origin, direction, CBSky.CenterRadius.w);
    if (ground.x > 0.001f) return 0;
    if (radius >= CBSky.Atmosphere.x)
    {
        float2 atmosphere = SkySphereRoots(origin, direction, CBSky.Atmosphere.x);
        if (atmosphere.x < 0) return 1;
        origin += direction * (atmosphere.x + 0.001f);
        radius = length(origin);
    }
    uint width, height;
    SkyTransmittance.GetDimensions(width, height);
    float2 uv = SkyTransmittanceUV(radius, dot(origin, direction) / radius,
        CBSky.CenterRadius.w, CBSky.Atmosphere.x, uint2(width, height));
    return SkyTransmittance.SampleLevel(SkySampler, uv, 0).rgb;
}
float3 SkySunDisk(float3 direction, float3 worldCm)
{
    if (CBSky.Mode.x != 2 || dot(direction, CBSky.SunDirection.xyz) < cos(CBSky.Atmosphere.z)) return 0;
    float solidAngle = SKY_PI * pow(sin(CBSky.Atmosphere.z), 2);
    return CBSky.SunIlluminance.rgb * SkySunTransmittance(worldCm, direction) / solidAngle;
}
#ifdef SKY_AERIAL_RADIANCE_REGISTER
Texture3D<float4> SkyAerialRadiance : register(SKY_AERIAL_RADIANCE_REGISTER);
Texture3D<float4> SkyAerialTransmittance : register(SKY_AERIAL_TRANSMITTANCE_REGISTER);
float3 ApplySkyAerialPerspective(float3 color, float2 uv, float distanceCm)
{
    if (CBSky.Mode.x != 2) return color;
    uint width, height, depth;
    SkyAerialRadiance.GetDimensions(width, height, depth);
    float z = sqrt(saturate(distanceCm * 0.00001f / CBSky.Atmosphere.y));
    // Fade from the identity at zero distance to the first volume texel.
    float weight = saturate(z * depth * 2);
    float3 coordinate = float3(uv, z);
    float3 radiance = SkyAerialRadiance.SampleLevel(SkySampler, coordinate, 0).rgb * weight;
    float3 transmittance = lerp(1, SkyAerialTransmittance.SampleLevel(SkySampler, coordinate, 0).rgb, weight);
    return color * transmittance + radiance;
}
#endif
#endif
