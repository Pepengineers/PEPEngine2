#ifndef REFLECTION_PROBES_HLSLI
#define REFLECTION_PROBES_HLSLI
struct ReflectionProbeData
{
    float3 Position; float Intensity;
    float3 BoxMin; float BlendDistance;
    float3 BoxMax; float MaxMip;
    float MinMip;
};

#ifdef REFLECTION_PROBE_BINDINGS_DEFERRED
TextureCubeArray<float4> LocalReflectionRadiance : register(t12);
StructuredBuffer<ReflectionProbeData> ReflectionProbes : register(t13);
#else
TextureCubeArray<float4> LocalReflectionRadiance : register(t9);
StructuredBuffer<ReflectionProbeData> ReflectionProbes : register(t10);
#endif

float3 BoxProjectedDirection(float3 position, float3 direction, ReflectionProbeData probe)
{
    float3 exitPlane = float3(direction.x >= 0 ? probe.BoxMax.x : probe.BoxMin.x,
        direction.y >= 0 ? probe.BoxMax.y : probe.BoxMin.y,
        direction.z >= 0 ? probe.BoxMax.z : probe.BoxMin.z);
    float3 distance = float3(abs(direction.x) > 1e-6f ? (exitPlane.x - position.x) / direction.x : 1e30f,
        abs(direction.y) > 1e-6f ? (exitPlane.y - position.y) / direction.y : 1e30f,
        abs(direction.z) > 1e-6f ? (exitPlane.z - position.z) / direction.z : 1e30f);
    return position + direction * min(distance.x, min(distance.y, distance.z)) - probe.Position;
}

// Returns linear, prefiltered incident radiance. Apply the split-sum BRDF once, at the caller.
float3 SampleReflectionRadiance(float3 position, float3 direction, float roughness, float3 globalRadiance)
{
    float3 localRadiance = 0;
    float totalWeight = 0;
    uint count, stride;
    ReflectionProbes.GetDimensions(count, stride);
    for (uint i = 0; i < count; ++i)
    {
        ReflectionProbeData probe = ReflectionProbes[i];
        if (probe.Intensity <= 0 || any(position < probe.BoxMin) || any(position > probe.BoxMax)) continue;
        float3 edge = min(position - probe.BoxMin, probe.BoxMax - position);
        float weight = saturate(min(edge.x, min(edge.y, edge.z)) / probe.BlendDistance);
        float3 lookup = BoxProjectedDirection(position, direction, probe);
        if (dot(lookup, lookup) < 1e-12f) lookup = direction;
        localRadiance += LocalReflectionRadiance.SampleLevel(samLinearClamp,
            float4(lookup, i), lerp(probe.MinMip, probe.MaxMip, saturate(roughness))).rgb * probe.Intensity * weight;
        totalWeight += weight;
    }
    return lerp(globalRadiance, localRadiance / max(totalWeight, 1e-6f), saturate(totalWeight));
}
#endif
