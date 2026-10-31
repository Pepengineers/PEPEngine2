#ifndef PROBE_IBL_HLSLI
#define PROBE_IBL_HLSLI

float3 GlobalSpecularRadiance(float3 direction, float roughness)
{
    uint width, height, levels;
    PrefilteredEnvironment.GetDimensions(0, width, height, levels);
    return PrefilteredEnvironment.SampleLevel(samLinearClamp, direction, roughness * (levels - 1)).rgb;
}

#if defined(PROBE_LIGHTING)
struct ReflectionProbe
{
    float3 Position; uint CubeIndex;
    float3 Extents; uint MipCount;
};
cbuffer ReflectionProbeGlobals : register(PROBE_GLOBALS_REGISTER) { uint ReflectionProbeCount; };
StructuredBuffer<ReflectionProbe> ReflectionProbes : register(PROBE_DATA_REGISTER);
TextureCube<float4> ReflectionProbeCubes[TEXTURECUBE_RANGE_LENGTH] : register(PROBE_CUBES_REGISTER);

float3 BoxProjectedDirection(float3 position, float3 direction, ReflectionProbe probe)
{
    float3 signs = float3(direction.x >= 0 ? 1 : -1, direction.y >= 0 ? 1 : -1, direction.z >= 0 ? 1 : -1);
    float3 reciprocal = signs / max(abs(direction), 1e-6);
    float3 a = (probe.Position - probe.Extents - position) * reciprocal;
    float3 b = (probe.Position + probe.Extents - position) * reciprocal;
    float3 farIntersection = max(a, b);
    float distance = min(farIntersection.x, min(farIntersection.y, farIntersection.z));
    return position + direction * distance - probe.Position;
}
float3 ProbeSpecularRadiance(float3 position, float3 direction, float roughness, out float totalWeight)
{
    float3 radiance = 0;
    totalWeight = 0;
    for (uint i = 0; i < ReflectionProbeCount; ++i)
    {
        ReflectionProbe probe = ReflectionProbes[i];
        float3 relative = abs(position - probe.Position) / probe.Extents;
        float weight = saturate(1 - max(relative.x, max(relative.y, relative.z)));
        if (weight <= 0) continue;
        radiance += weight * ReflectionProbeCubes[NonUniformResourceIndex(probe.CubeIndex)].SampleLevel(
            samLinearClamp, BoxProjectedDirection(position, direction, probe), roughness * (probe.MipCount - 1)).rgb;
        totalWeight += weight;
    }
    return radiance;
}
#endif

float3 SpecularEnvironmentRadiance(float3 position, float3 direction, float roughness)
{
#if defined(PROBE_LIGHTING)
    float weight;
    float3 local = ProbeSpecularRadiance(position, direction, roughness, weight);
    if (weight > 0)
        return (local + saturate(1 - weight) * GlobalSpecularRadiance(direction, roughness)) / max(1, weight);
#endif
    return GlobalSpecularRadiance(direction, roughness);
}
#endif
