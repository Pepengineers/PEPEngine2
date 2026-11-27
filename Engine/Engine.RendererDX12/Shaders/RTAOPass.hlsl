#include "RayTracing.hlsli"
#include "../../../External/External.Libraries/nrd/Shaders/NRD.hlsli"

cbuffer Parameters : register(b0)
{
    float Radius, Intensity, Bias;
    uint Samples;
    float FadeStart, FadeEnd;
    uint FrameIndex;
};
ConstantBuffer<CameraCB> Camera : register(b1);
Texture2D<float> Depth : register(t0);
Texture2D<float4> Normals : register(t1);
RaytracingAccelerationStructure Scene : register(t2);
StructuredBuffer<GeometryBufferVertex> Vertices : register(t3);
Buffer<uint> Indices : register(t4);
StructuredBuffer<IndirectDrawArgs> Draws : register(t5);
StructuredBuffer<InstanceData> Instances : register(t6);
StructuredBuffer<Transform> Transforms : register(t7);
StructuredBuffer<Material> Materials : register(t8);
Texture2D<float> DenoisedOcclusion : register(t9);
Texture2D<float4> Texture2DCache[TEXTURE2D_RANGE_LENGTH] : register(t10);
RWTexture2D<float> HitDistance : register(u0);
RWTexture2D<float4> PackedNormals : register(u1);
RWTexture2D<float> ViewDepth : register(u2);
RWTexture2D<float> OutputAO : register(u3);
SamplerState samLinearWrap : register(s2);

float TraceOccluder(float3 origin, float3 direction)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0;
    ray.TMax = Radius;
    RayQuery<RAY_FLAG_FORCE_NON_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> query;
    query.TraceRayInline(Scene, RAY_FLAG_NONE, 0xff, ray);
    while (query.Proceed())
    {
        if (query.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE) continue;
        const uint instanceIndex = query.CandidateInstanceID() + query.CandidateGeometryIndex();
        const Material material = Materials[Instances[instanceIndex].MaterialIndex];
        if (material.RenderLayer == RENDER_LAYER_TRANSPARENT) continue;
        if (material.RenderLayer == RENDER_LAYER_MASKED && material.DiffuseMapIndex != NO_TEXTURE)
        {
            const RayTracingHitSurface surface = LoadRayTracingHitSurface(Vertices, Indices, Draws, Instances, Transforms,
                query.CandidateInstanceID(), query.CandidateGeometryIndex(), query.CandidatePrimitiveIndex(), query.CandidateTriangleBarycentrics());
            // Ray candidates have no raster derivatives, read the alpha mask at explicit mip zero.
            const float alpha = Texture2DCache[NonUniformResourceIndex(material.DiffuseMapIndex)].SampleLevel(samLinearWrap, surface.TexCoord, 0).a;
            if (alpha < 0.5f) continue;
        }
        query.CommitNonOpaqueTriangleHit();
    }
    return query.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? query.CommittedRayT() : Radius;
}

[numthreads(8, 8, 1)]
void CS(uint3 thread : SV_DispatchThreadID)
{
    uint2 size, pixel = thread.xy;
    Depth.GetDimensions(size.x, size.y);
    if (any(pixel >= size)) return;
    const float depth = Depth[pixel];
    const float viewDepth = Camera.NearPlane * Camera.FarPlane / (Camera.NearPlane + depth * (Camera.FarPlane - Camera.NearPlane));
    const float3 normal = SafeNormalize(Normals[pixel].xyz);
    const bool valid = depth > 0 && viewDepth < FadeEnd && dot(normal, normal) > 0.5f;
    PackedNormals[pixel] = NRD_FrontEnd_PackNormalAndRoughness(valid ? normal : float3(0, 0, 1), 1, 0);
    ViewDepth[pixel] = valid ? viewDepth : FadeEnd + 1;
    if (!valid || Intensity == 0)
    {
        HitDistance[pixel] = 1;
        return;
    }
    const float2 ndc = (float2(pixel) + 0.5f) / size * float2(2, -2) + float2(-1, 1);
    const float4 position = mul(float4(ndc, depth, 1), Camera.InvViewProj);
    const float3 origin = position.xyz / position.w + normal * Bias;
    const float3 tangent = normalize(cross(abs(normal.z) < 0.99f ? float3(0, 0, 1) : float3(0, 1, 0), normal));
    const float3 bitangent = cross(normal, tangent);
    const float rotation = frac(52.9829189f * frac(dot(float2(pixel), float2(0.06711056f, 0.00583715f)))) * 6.28318530718f;
    const float2 temporalOffset = frac(float(FrameIndex % 4096) * float2(0.754877666f, 0.569840291f));
    // Separate radial phases prevent temporal sampling from pulsing across whole surfaces.
    uint pixelSeed = pixel.x + pixel.y * size.x + 1;
    pixelSeed ^= pixelSeed >> 16;
    pixelSeed *= 0x7feb352du;
    pixelSeed ^= pixelSeed >> 15;
    pixelSeed *= 0x846ca68bu;
    pixelSeed ^= pixelSeed >> 16;
    const float radialOffset = float(pixelSeed & 0xffffffu) / 16777216.0f;
    float hitDistance = 0;
    for (uint sampleIndex = 0; sampleIndex < Samples; ++sampleIndex)
    {
        const float radial = frac((sampleIndex + 0.5f) / Samples + temporalOffset.x + radialOffset);
        const float angle = sampleIndex * 2.39996323f + rotation + temporalOffset.y * 6.28318530718f;
        const float3 direction = tangent * (cos(angle) * sqrt(radial)) + bitangent * (sin(angle) * sqrt(radial)) + normal * sqrt(1 - radial);
        hitDistance += REBLUR_FrontEnd_GetNormHitDist(TraceOccluder(origin, direction), viewDepth, float3(Radius, 0, 1), 1);
    }
    HitDistance[pixel] = max(hitDistance / Samples, 1e-5f);
}

[numthreads(8, 8, 1)]
void ResolveCS(uint3 thread : SV_DispatchThreadID)
{
    uint2 size, pixel = thread.xy;
    Depth.GetDimensions(size.x, size.y);
    if (any(pixel >= size)) return;
    const float depth = Depth[pixel];
    const float viewDepth = Camera.NearPlane * Camera.FarPlane / (Camera.NearPlane + depth * (Camera.FarPlane - Camera.NearPlane));
    if (depth <= 0 || viewDepth >= FadeEnd || Intensity == 0 || dot(Normals[pixel].xyz, Normals[pixel].xyz) < 0.5f)
    {
        OutputAO[pixel] = 1;
        return;
    }
    const float visibility = pow(saturate(DenoisedOcclusion[pixel]), Intensity);
    const float fade = saturate((viewDepth - FadeStart) / (FadeEnd - FadeStart));
    OutputAO[pixel] = lerp(visibility, 1, fade);
}
