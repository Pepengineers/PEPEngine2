#include "CBufferStructures.hlsl"
#include "FluidParameters.hlsli"
cbuffer DispatchParameters : register(b0)
{
    uint2 Size; uint OwnerId; uint FilterMode;
};
ConstantBuffer<CameraCB> Camera : register(b2);
SamplerState samLinearClamp : register(s3);
StructuredBuffer<FluidParticle> Particles : register(t0);
Texture2D<float4> Raw : register(t1);
Texture2D<float> Filtered : register(t2);
Texture2D<float> Thickness : register(t3);
Texture2D<float> OpaqueDepth : register(t4);
Texture2D<float2> OpaqueMotion : register(t5);
Texture2D<float4> Background : register(t6);
TextureCube<float4> Environment : register(t7);
#include "ReflectionProbes.hlsli"
Texture2D<float4> Geometry : register(t11);
Texture2D<float> WaterDepth : register(t12);
Texture2D<uint> Owner : register(t13);
RWTexture2D<float> DepthOut : register(u0);
RWTexture2D<float2> MotionOut : register(u1);
RWTexture2D<float4> GeometryOut : register(u2);
RWTexture2D<uint> OwnerOut : register(u3);
RWTexture2D<float> FilterOut : register(u4);
RWTexture2D<float4> ColorOut : register(u5);

float3 Ray(float2 pixel)
{
    float2 ndc = pixel / Size * float2(2, -2) + float2(-1, 1);
    // Homogeneous far rays avoid subtracting nearly equal world-space near points.
    float4 world = mul(float4(ndc, 0, 1), Camera.InvViewProj);
    return normalize(world.xyz - Camera.CameraLocation * world.w);
}
float3 WorldAt(float2 pixel, float eyeDepth)
{
    float3 ray = Ray(pixel);
    float viewZ = -mul(float4(ray, 0), Camera.View).z;
    return Camera.CameraLocation + ray * (eyeDepth / max(viewZ, 1e-6f));
}
float DeviceDepth(float3 world)
{
    float4 clip = mul(float4(world, 1), Camera.ViewProj);
    return clip.z / clip.w;
}
float DistanceAt(float2 pixel, float depth)
{
    float2 ndc = pixel / Size * float2(2, -2) + float2(-1, 1);
    float4 world = mul(float4(ndc, depth, 1), Camera.InvViewProj);
    return length(world.xyz / world.w - Camera.CameraLocation);
}
struct SphereVertex
{
    float4 Position : SV_POSITION;
    nointerpolation uint ParticleId : TEXCOORD0;
};
SphereVertex SphereVS(uint vertex : SV_VertexID, uint instance : SV_InstanceID)
{
    static const float2 Corners[6] = { float2(-1,-1), float2(-1,1), float2(1,1),
        float2(-1,-1), float2(1,1), float2(1,-1) };
    float3 center = Particles[instance].Position;
    float viewDepth = -mul(float4(center, 1), Camera.View).z;
    // Conservative projected bound; the fragment shader intersects the actual sphere.
    float halfSize = Radius * max(viewDepth, Camera.NearPlane) / max(viewDepth - Radius, Camera.NearPlane);
    float3 right = float3(Camera.View._11, Camera.View._21, Camera.View._31);
    float3 up = float3(Camera.View._12, Camera.View._22, Camera.View._32);
    float3 surfacePoint = center + (right * Corners[vertex].x + up * Corners[vertex].y) * halfSize;
    SphereVertex output;
    output.Position = mul(float4(surfacePoint, 1), Camera.ViewProj);
    if (viewDepth - Radius <= Camera.NearPlane) output.Position = float4(Corners[vertex], 0.5f, 1);
    if (viewDepth + Radius <= Camera.NearPlane) output.Position = float4(2, 2, 0, 1);
    output.ParticleId = instance;
    return output;
}
bool Intersect(SphereVertex input, out float front, out float back, out float3 ray)
{
    ray = Ray(input.Position.xy);
    float3 offset = Camera.CameraLocation - Particles[input.ParticleId].Position;
    float b = dot(offset, ray);
    float c = dot(offset, offset) - Radius * Radius;
    float discriminant = b * b - c;
    front = -b - sqrt(max(0, discriminant));
    back = -b + sqrt(max(0, discriminant));
    float nearDistance = Camera.NearPlane / max(-mul(float4(ray, 0), Camera.View).z, 1e-6f);
    front = max(front, nearDistance);
    float opaque = OpaqueDepth[(uint2)input.Position.xy];
    if (opaque > 0) back = min(back, DistanceAt(input.Position.xy, opaque));
    return discriminant >= 0 && back > front;
}
struct SurfaceOutput
{
    float4 RawData : SV_TARGET0;
    float EyeDepth : SV_TARGET1;
    float Depth : SV_DEPTH;
};
SurfaceOutput SpherePS(SphereVertex input)
{
    float front, back; float3 ray;
    if (!Intersect(input, front, back, ray)) discard;
    float3 world = Camera.CameraLocation + ray * front;
    FluidParticle particle = Particles[input.ParticleId];
    SurfaceOutput output;
    output.EyeDepth = -mul(float4(world, 1), Camera.View).z;
    output.RawData = float4(output.EyeDepth, particle.Position - particle.PreviousPosition);
    output.Depth = DeviceDepth(world);
    return output;
}
float ThicknessPS(SphereVertex input) : SV_TARGET0
{
    float front, back; float3 ray;
    if (!Intersect(input, front, back, ray)) discard;
    return min(back - front, MaxThickness);
}
[numthreads(8, 8, 1)]
void InitializeSurface(uint2 p : SV_DispatchThreadID)
{
    if (any(p >= Size)) return;
    DepthOut[p] = OpaqueDepth[p];
    MotionOut[p] = OpaqueMotion[p];
    GeometryOut[p] = 0;
    OwnerOut[p] = 0;
}
// Truong/Yuksel 2018 equations 2, 6, 8, 9, expressed with positive eye depth.
void FilterPair(int2 p, int2 offset, float center, float sigma, inout float lower,
    inout float upper, inout float sum, inout float weightSum)
{
    int2 a = p + offset, b = p - offset;
    // Treat the viewport edge symmetrically, like the foreground rejection below.
    if (any(a < 0) || any(b < 0) || any(a >= (int2)Size) || any(b >= (int2)Size)) return;
    float za = Filtered[a], zb = Filtered[b];
    if (za > 0 && za >= center - lower && za <= center + upper)
    {
        lower = max(lower, center - za + DepthRange);
        upper = max(upper, za - center + DepthRange);
    }
    if (zb > 0 && zb >= center - lower && zb <= center + upper)
    {
        lower = max(lower, center - zb + DepthRange);
        upper = max(upper, zb - center + DepthRange);
    }
    if ((za > 0 && za < center - lower) || (zb > 0 && zb < center - lower)) return;
    za = za == 0 || za > center + upper ? center + EdgeDepth : za;
    zb = zb == 0 || zb > center + upper ? center + EdgeDepth : zb;
    float weight = exp(-dot(float2(offset), float2(offset)) / (2 * sigma * sigma));
    sum += (za + zb) * weight;
    weightSum += 2 * weight;
}
[numthreads(8, 8, 1)]
void NarrowRange(uint2 p : SV_DispatchThreadID)
{
    if (any(p >= Size)) return;
    float center = Filtered[p];
    if (center <= 0) { FilterOut[p] = 0; return; }
    // Derive focal length from adjacent camera rays; works with raster jitter.
    float3 r0 = Ray(float2(Size) * 0.5f);
    float3 r1 = Ray(float2(Size) * 0.5f + float2(0, 1));
    float focalPixels = rcp(max(length(r1 - r0), 1e-6f));
    float sigma = max(0.5f, ceil(focalPixels * Sigma / center));
    int radius = min((int)FilterRadius, (int)ceil(3 * sigma));
    float lower = DepthRange, upper = DepthRange;
    float sum = center, weightSum = 1;
    if (FilterMode < 2)
    {
        for (int distance = 1; distance <= radius; ++distance)
            FilterPair(p, FilterMode == 0 ? int2(distance, 0) : int2(0, distance),
                center, sigma, lower, upper, sum, weightSum);
    }
    else
    {
        // Small 5x5 2D cleanup following the separable passes (paper section 3.4).
        static const int2 Pairs[12] = { int2(1,0), int2(0,1), int2(1,1), int2(-1,1),
            int2(2,0), int2(0,2), int2(2,1), int2(-2,1), int2(1,2), int2(-1,2), int2(2,2), int2(-2,2) };
        for (uint pair = 0; pair < 12; ++pair)
            FilterPair(p, Pairs[pair], center, sigma, lower, upper, sum, weightSum);
    }
    FilterOut[p] = max(0.0001f, sum / weightSum);
}
float3 NeighborWorld(int2 p, int2 offset, float center)
{
    int2 q = clamp(p + offset, int2(0, 0), int2(Size) - 1);
    float z = Filtered[q];
    if (z <= 0 || abs(z - center) > DepthRange) return WorldAt(float2(q) + 0.5f, center);
    return WorldAt(float2(q) + 0.5f, z);
}
[numthreads(8, 8, 1)]
void MergeSurface(uint2 p : SV_DispatchThreadID)
{
    if (any(p >= Size)) return;
    float z = Filtered[p];
    if (z <= 0) return;
    float3 world = WorldAt(float2(p) + 0.5f, z);
    float depth = DeviceDepth(world);
    // shortcut: one nearest interface per pixel; use layered surfaces for nested fluids.
    if (depth <= DepthOut[p] || depth > 1 || depth <= 0) return;
    float3 left = world - NeighborWorld(p, int2(-1, 0), z);
    float3 right = NeighborWorld(p, int2(1, 0), z) - world;
    float3 up = world - NeighborWorld(p, int2(0, -1), z);
    float3 down = NeighborWorld(p, int2(0, 1), z) - world;
    float3 dx = dot(left, left) < dot(right, right) ? left : right;
    float3 dy = dot(up, up) < dot(down, down) ? up : down;
    float3 normal = cross(dx, dy);
    normal = dot(normal, normal) > 1e-12f ? normalize(normal) : -Ray(float2(p) + 0.5f);
    if (dot(normal, Camera.CameraLocation - world) < 0) normal = -normal;
    float3 previousWorld = world - Raw[p].yzw;
    float4 current = mul(float4(world, 1), Camera.ViewProjNoJitter);
    float4 previous = mul(float4(previousWorld, 1), Camera.PrevViewProjNoJitter);
    float2 motion = previous.w > 1e-5f && current.w > 1e-5f && ResetMotion == 0 ?
        previous.xy / previous.w - current.xy / current.w : 0;
    DepthOut[p] = depth;
    MotionOut[p] = all(isfinite(motion)) ? motion : 0;
    GeometryOut[p] = float4(normal, min(Thickness[p], MaxThickness));
    OwnerOut[p] = OwnerId;
}
[numthreads(8, 8, 1)]
void ResolveWater(uint2 p : SV_DispatchThreadID)
{
    if (any(p >= Size) || Owner[p] != OwnerId) return;
    float depth = WaterDepth[p];
    float2 pixel = float2(p) + 0.5f;
    float3 ray = Ray(pixel);
    float3 world = Camera.CameraLocation + ray * DistanceAt(pixel, depth);
    float4 geometry = Geometry[p];
    float3 normal = geometry.xyz;
    float2 uv = pixel / Size;
    float3 refracted = refract(ray, normal, rcp(IOR));
    float3 sampleWorld = world + refracted * max(geometry.w, Radius);
    float4 sampleClip = mul(float4(sampleWorld, 1), Camera.ViewProj);
    float2 sampleUV = sampleClip.w > 1e-5f ? sampleClip.xy / sampleClip.w * float2(0.5f, -0.5f) + 0.5f : uv;
    float2 offsetPixels = (sampleUV - uv) * Size;
    offsetPixels *= min(1.0f, RefractionPixels / max(length(offsetPixels), 1e-6f));
    float2 refractedUV = uv + offsetPixels / Size;
    refractedUV = clamp(refractedUV, 0.5f / Size, 1 - 0.5f / Size);
    uint2 q = min((uint2)(refractedUV * Size), Size - 1);
    // Reject samples belonging to opaque geometry in front of this water surface.
    if (OpaqueDepth[q] >= depth) { refractedUV = uv; q = p; }
    float thickness = geometry.w;
    if (OpaqueDepth[q] > 0)
        thickness = min(thickness, max(0, DistanceAt(float2(q) + 0.5f, OpaqueDepth[q]) - length(world - Camera.CameraLocation)));
    float3 transmission = exp(-Absorption * min(thickness, MaxThickness));
    float3 transmitted = Background.SampleLevel(samLinearClamp, refractedUV, 0).rgb * transmission +
        Scattering * (1 - transmission);
    float3 reflectionDirection = reflect(ray, normal);
    uint w, h, levels;
    Environment.GetDimensions(0, w, h, levels);
    float3 environment = Environment.SampleLevel(samLinearClamp, reflectionDirection, Roughness * (levels - 1)).rgb;
    float3 reflected = SampleReflectionRadiance(world, reflectionDirection, Roughness, environment);
    float cosI = saturate(dot(-ray, normal));
    float cosT = sqrt(max(0, 1 - (1 - cosI * cosI) / (IOR * IOR)));
    float rs = (cosI - IOR * cosT) / max(cosI + IOR * cosT, 1e-6f);
    float rp = (IOR * cosI - cosT) / max(IOR * cosI + cosT, 1e-6f);
    float fresnel = 0.5f * (rs * rs + rp * rp);
    ColorOut[p] = float4(lerp(transmitted, reflected, fresnel), 1);
}
