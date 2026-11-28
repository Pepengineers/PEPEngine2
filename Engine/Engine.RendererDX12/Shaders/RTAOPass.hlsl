#include "RayTracing.hlsli"
#include "../../../External/External.Libraries/nrd/Shaders/NRD.hlsli"

cbuffer Parameters : register(b0)
{
    float Radius, Intensity, Bias;
    uint SamplesPerPixel;
    float FadeStart, FadeEnd;
    uint FrameIndex, CheckerboardTracing;
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
Texture2D<float2> Velocity : register(t10);
Texture2D<float4> Texture2DCache[TEXTURE2D_RANGE_LENGTH] : register(t11);
RWTexture2D<float> HitDistance : register(u0);
RWTexture2D<float4> PackedNormals : register(u1);
RWTexture2D<float> ViewDepth : register(u2);
RWTexture2D<float> OutputAO : register(u3);
RWTexture2D<float2> MotionVectors : register(u4);
SamplerState samLinearWrap : register(s2);

uint2 GetSourcePixel(uint2 occlusionPixel, uint2 occlusionSize, uint2 sourceSize)
{
    return min(uint2((float2(occlusionPixel) + 0.5f) * sourceSize / occlusionSize), sourceSize - 1);
}

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
    uint2 occlusionSize, occlusionPixel = thread.xy, size;
    HitDistance.GetDimensions(occlusionSize.x, occlusionSize.y);
    if (any(occlusionPixel >= occlusionSize)) return;
    Depth.GetDimensions(size.x, size.y);
    uint2 pixel = GetSourcePixel(occlusionPixel, occlusionSize, size);
    MotionVectors[occlusionPixel] = Velocity[pixel];
    const float depth = Depth[pixel];
    const float viewDepth = Camera.NearPlane * Camera.FarPlane / (Camera.NearPlane + depth * (Camera.FarPlane - Camera.NearPlane));
    const float3 normal = SafeNormalize(Normals[pixel].xyz);
    const bool valid = depth > 0 && viewDepth < FadeEnd && dot(normal, normal) > 0.5f;
    PackedNormals[occlusionPixel] = NRD_FrontEnd_PackNormalAndRoughness(valid ? normal : float3(0, 0, 1), 1, 0);
    ViewDepth[occlusionPixel] = valid ? viewDepth : FadeEnd + 1;
    uint2 signalPixel = occlusionPixel;
    if (CheckerboardTracing)
    {
        signalPixel.x >>= 1;
        if ((occlusionPixel.x ^ occlusionPixel.y ^ FrameIndex) & 1)
        {
            // Odd widths leave one packed cell without a sample on alternating rows.
            if ((occlusionSize.x & 1) && occlusionPixel.x == occlusionSize.x - 1) HitDistance[signalPixel] = 0;
            return;
        }
    }
    if (!valid || Intensity == 0)
    {
        HitDistance[signalPixel] = 1;
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
    for (uint sampleIndex = 0; sampleIndex < SamplesPerPixel; ++sampleIndex)
    {
        const float radial = frac((sampleIndex + 0.5f) / SamplesPerPixel + temporalOffset.x + radialOffset);
        const float angle = sampleIndex * 2.39996323f + rotation + temporalOffset.y * 6.28318530718f;
        const float3 direction = tangent * (cos(angle) * sqrt(radial)) + bitangent * (sin(angle) * sqrt(radial)) + normal * sqrt(1 - radial);
        hitDistance += REBLUR_FrontEnd_GetNormHitDist(TraceOccluder(origin, direction), viewDepth, float3(Radius, 0, 1), 1);
    }
    HitDistance[signalPixel] = max(hitDistance / SamplesPerPixel, 1e-5f);
}

float ResolveOcclusion(uint2 pixel, uint2 size, float viewDepth, float3 normal)
{
    uint2 occlusionSize;
    DenoisedOcclusion.GetDimensions(occlusionSize.x, occlusionSize.y);
    if (all(occlusionSize == size)) return DenoisedOcclusion[pixel];
    float2 position = (float2(pixel) + 0.5f) * occlusionSize / size - 0.5f;
    int2 origin = int2(floor(position));
    float2 fraction = frac(position);
    float occlusion = 0, totalWeight = 0;
    // Reject samples from other surfaces when restoring full-resolution AO.
    [unroll] for (int y = 0; y < 2; ++y)
    [unroll] for (int x = 0; x < 2; ++x)
    {
        uint2 samplePixel = uint2(clamp(origin + int2(x, y), int2(0, 0), int2(occlusionSize) - 1));
        uint2 sourcePixel = GetSourcePixel(samplePixel, occlusionSize, size);
        float sampleDepth = Depth[sourcePixel];
        float sampleViewDepth = Camera.NearPlane * Camera.FarPlane / (Camera.NearPlane + sampleDepth * (Camera.FarPlane - Camera.NearPlane));
        float3 sampleNormal = Normals[sourcePixel].xyz;
        if (sampleDepth <= 0 || sampleViewDepth >= FadeEnd || dot(sampleNormal, sampleNormal) < 0.5f) continue;
        float weight = (x ? fraction.x : 1 - fraction.x) * (y ? fraction.y : 1 - fraction.y);
        weight *= exp2(-abs(viewDepth - sampleViewDepth) / max(viewDepth * 0.02f, 1e-3f));
        weight *= pow(saturate(dot(normal, SafeNormalize(sampleNormal))), 32);
        occlusion += weight * DenoisedOcclusion[samplePixel];
        totalWeight += weight;
    }
    return totalWeight > 1e-4f ? occlusion / totalWeight : 1;
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
    const float visibility = pow(saturate(ResolveOcclusion(pixel, size, viewDepth, SafeNormalize(Normals[pixel].xyz))), Intensity);
    const float fade = saturate((viewDepth - FadeStart) / (FadeEnd - FadeStart));
    OutputAO[pixel] = lerp(visibility, 1, fade);
}
