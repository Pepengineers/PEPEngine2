#ifndef RAY_TRACING_HLSLI
#define RAY_TRACING_HLSLI

#include "CBufferStructures.hlsli"

struct RayTracingHitSurface
{
    uint InstanceCacheIndex;
    uint MaterialIndex;
    float3 WorldPosition;
    float3 WorldNormal;
    float2 TexCoord;
};

// Creates all data you need from a ray trace result.
//
// TLAS InstanceID is the first InstanceCache index allocated by BindMesh.
// BLAS geometry order matches the mesh's submeshes, so their existing contiguous entries are reused.
// Read the original IndirectCommandsCache, camera and shadow culled DrawStreams are not hit tables.
RayTracingHitSurface LoadRayTracingHitSurface(StructuredBuffer<GeometryBufferVertex> vertices, Buffer<uint> indices,
    StructuredBuffer<IndirectDrawArgs> draws, StructuredBuffer<InstanceData> instances, StructuredBuffer<Transform> transforms,
    uint instanceID, uint geometryIndex, uint primitiveIndex, float2 barycentrics)
{
    RayTracingHitSurface surface;
    surface.InstanceCacheIndex = instanceID + geometryIndex;
    const InstanceData instance = instances[surface.InstanceCacheIndex];
    const IndirectDrawArgs draw = draws[surface.InstanceCacheIndex];
    const uint firstIndex = draw.StartIndexLocation + primitiveIndex * 3;
    const GeometryBufferVertex v0 = vertices[int(indices[firstIndex]) + draw.BaseVertexLocation];
    const GeometryBufferVertex v1 = vertices[int(indices[firstIndex + 1]) + draw.BaseVertexLocation];
    const GeometryBufferVertex v2 = vertices[int(indices[firstIndex + 2]) + draw.BaseVertexLocation];
    const float3 weights = float3(1.0f - barycentrics.x - barycentrics.y, barycentrics);
    const float3 position = v0.Position * weights.x + v1.Position * weights.y + v2.Position * weights.z;
    const Transform transform = transforms[instance.TransformIndex];
    surface.MaterialIndex = instance.MaterialIndex;
    surface.WorldPosition = mul(float4(position, 1.0f), transform.World).xyz;
    // Match raster VS normal transformation followed by PS interpolation and normalization.
    const float3 n0 = TransformNormal(v0.Normal, (float3x3)transform.World);
    const float3 n1 = TransformNormal(v1.Normal, (float3x3)transform.World);
    const float3 n2 = TransformNormal(v2.Normal, (float3x3)transform.World);
    surface.WorldNormal = SafeNormalize(n0 * weights.x + n1 * weights.y + n2 * weights.z);
    surface.TexCoord = v0.TexCoord * weights.x + v1.TexCoord * weights.y + v2.TexCoord * weights.z;
    return surface;
}

#endif

