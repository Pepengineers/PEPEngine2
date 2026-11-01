// Guertin, McGuire, Nowrouzezahrai: A Fast and Stable Feature-Aware Motion
// Blur Filter, HPG 2014, Appendix A. Independently implemented; see docs/motion-blur.md.
#include "CBufferStructures.hlsl"

cbuffer Parameters : register(b0)
{
    uint2 Size;
    uint TileSize;
    uint Samples;
    float ShutterFraction;
    float MaxRadius;
    uint BlurEnabled;
    uint ResetMotion;
    float2 OutputSize;
    float CenterWeight;
    float VelocityThreshold;
    uint HasFeatureMotion;
    uint3 Padding;
};
ConstantBuffer<CameraCB> Camera : register(b1);
Texture2D<float4> Color : register(t0);
Texture2D<float> Depth : register(t1);
Texture2D<float2> Motion : register(t2);
Texture2D<float2> Velocity : register(t3);
Texture2D<float2> TileMax : register(t4);
Texture2D<float2> NeighborMax : register(t5);
Texture2D<float4> FeatureMotion : register(t6); // weighted NDC motion, normalized view depth, coverage
RWTexture2D<float4> Result : register(u0);
RWTexture2D<float2> CompleteMotion : register(u1);
RWTexture2D<float2> PreparedVelocity : register(u2);
RWTexture2D<float2> TileResult : register(u3);
RWTexture2D<float2> NeighborResult : register(u4);

uint2 TileCount() { return (Size + TileSize - 1) / TileSize; }

[numthreads(8, 8, 1)]
void Prepare(uint2 p : SV_DispatchThreadID)
{
    if (any(p >= Size)) return;
    float2 motion = Motion[p];
    if (Depth[p] == 0)
    {
        float2 ndc = (float2(p) + 0.5f) / Size * float2(2, -2) + float2(-1, 1);
        float4 world = mul(float4(ndc, 0, 1), Camera.InvViewProj);
        // Homogeneous direction at infinity removes all camera translation.
        float4 ray = float4(world.xyz - Camera.CameraLocation * world.w, 0);
        float4 current = mul(ray, Camera.ViewProjNoJitter);
        float4 previous = mul(ray, Camera.PrevViewProjNoJitter);
        motion = current.w > 0.00001f && previous.w > 0.00001f ?
            previous.xy / previous.w - current.xy / current.w : 0;
    }
    if (HasFeatureMotion != 0)
    {
        float4 feature = FeatureMotion[p];
        if (feature.w > 0.00001f) motion = lerp(motion, feature.xy / feature.w, saturate(feature.w));
    }
    if (ResetMotion != 0 || !all(isfinite(motion))) motion = 0;
    CompleteMotion[p] = motion;
    // NDC -> output pixels, then HALF of a centered shutter interval.
    float2 radius = motion * float2(0.5f, -0.5f) * OutputSize * ShutterFraction * 0.5f;
    radius *= min(1.0f, MaxRadius / max(length(radius), 0.00001f));
    PreparedVelocity[p] = BlurEnabled != 0 ? radius * Size / OutputSize : 0;
}

groupshared float2 TileVelocities[256];

[numthreads(256, 1, 1)]
void Tiles(uint2 tile : SV_GroupID, uint lane : SV_GroupIndex)
{
    float2 maximum = 0;
    for (uint i = lane; i < TileSize * TileSize; i += 256)
    {
        uint2 p = tile * TileSize + uint2(i % TileSize, i / TileSize);
        if (all(p < Size))
        {
            float2 v = Velocity[p];
            if (dot(v, v) > dot(maximum, maximum)) maximum = v;
        }
    }
    TileVelocities[lane] = maximum;
    GroupMemoryBarrierWithGroupSync();
    for (uint stride = 128; stride > 0; stride >>= 1)
    {
        if (lane < stride)
        {
            float2 v = TileVelocities[lane + stride];
            if (dot(v, v) > dot(TileVelocities[lane], TileVelocities[lane])) TileVelocities[lane] = v;
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (lane == 0) TileResult[tile] = TileVelocities[0];
}

[numthreads(8, 8, 1)]
void Neighbors(uint2 tile : SV_DispatchThreadID)
{
    uint2 count = TileCount();
    if (any(tile >= count)) return;
    float2 maximum = 0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
        {
            int2 neighbor = int2(tile) + int2(x, y);
            if (any(neighbor < 0) || any(neighbor >= int2(count))) continue;
            float2 v = TileMax[neighbor];
            // Both signs of the exposure can reach the center; reject off-axis corners.
            if (x != 0 && y != 0 && v.x * v.y * x * y <= 0) continue;
            if (dot(v, v) > dot(maximum, maximum)) maximum = v;
        }
    NeighborResult[tile] = maximum;
}

float Halton(uint index, uint base)
{
    float result = 0;
    float fraction = 1;
    while (index > 0)
    {
        fraction /= base;
        result += fraction * (index % base);
        index /= base;
    }
    return result;
}

float LinearDepth(int2 p)
{
    if (HasFeatureMotion != 0)
    {
        float4 feature = FeatureMotion[p];
        if (feature.w >= 0.5f)
            return clamp(feature.z / feature.w * Camera.FarPlane, Camera.NearPlane, Camera.FarPlane);
    }
    // RH finite reverse-Z: positive view distance in scene centimeters.
    float depth = Depth[p];
    return Camera.NearPlane * Camera.FarPlane /
        (Camera.NearPlane + depth * (Camera.FarPlane - Camera.NearPlane));
}

// Section 5 expressed in positive view distances (equivalent to comparing 1/z).
float ZCompare(float a, float b) { return saturate(1.0f - (b - a) / min(a, b)); }
float Cone(float distance, float radius) { return saturate(1.0f - distance / radius); }
float Cylinder(float distance, float radius) { return 1.0f - smoothstep(0.95f * radius, 1.05f * radius, distance); }

[numthreads(8, 8, 1)]
void Gather(uint2 p : SV_DispatchThreadID)
{
    if (any(p >= Size)) return;
    float4 center = Color[p];
    // Spatially fixed low discrepancy jitter avoids introducing temporal shimmer.
    uint sequence = 1 + p.x + p.y * Size.x;
    float jitter = Halton(sequence, 2) * 2.0f - 1.0f;
    int2 tile = p / TileSize;
    float2 edge = (float2(p % TileSize) + 0.5f) / TileSize - 0.5f;
    bool horizontal = abs(edge.x) > abs(edge.y);
    float boundary = horizontal ? edge.x : edge.y;
    if (Halton(sequence, 3) < max(0.0f, abs(boundary) * 2.0f - 0.5f))
        tile += horizontal ? int2(boundary > 0 ? 1 : -1, 0) : int2(0, boundary > 0 ? 1 : -1);
    tile = clamp(tile, 0, int2(TileCount()) - 1);
    float2 dominant = NeighborMax[tile];
    float extent = length(dominant);
    if (extent <= 0.5f)
    {
        Result[p] = center;
        return;
    }
    // Canonical line orientation makes centered exposure invariant to velocity sign.
    if (dominant.x < 0 || (dominant.x == 0 && dominant.y < 0)) dominant = -dominant;
    float2 wn = dominant / extent;
    float2 velocity = Velocity[p];
    float speed = length(velocity);
    if (dot(velocity, wn) < 0) velocity = -velocity;
    float2 perpendicular = float2(-wn.y, wn.x);
    if (dot(perpendicular, velocity) < 0) perpendicular = -perpendicular;
    float2 wc = normalize(lerp(perpendicular, velocity / max(speed, 0.5f), saturate((speed - 0.5f) / VelocityThreshold)));
    if (wc.x < 0 || (wc.x == 0 && wc.y < 0)) wc = -wc;
    float centerRadius = max(speed, 0.5f);
    float weightSum = Samples / (CenterWeight * centerRadius);
    float3 colorDelta = 0;
    float centerDepth = LinearDepth(p);
    for (uint i = 0; i < Samples; ++i)
    {
        float jitterScale = min(0.95f, 0.95f * 27.0f / Samples);
        float t = lerp(-1.0f, 1.0f, (i + 1.0f + jitter * jitterScale) / (Samples + 1.0f));
        float2 direction = (i & 1) != 0 ? wc : wn;
        float distance = abs(t) * extent;
        int2 samplePoint = int2(p) + int2(round(t * extent * direction));
        if (any(samplePoint < 0) || any(samplePoint >= int2(Size))) continue;
        float2 sampleVelocity = Velocity[samplePoint];
        float sampleRadius = max(length(sampleVelocity), 0.5f);
        float sampleDepth = LinearDepth(samplePoint);
        float wA = abs(dot(wc, direction));
        float wB = abs(dot(sampleVelocity / sampleRadius, direction));
        float weight = ZCompare(centerDepth, sampleDepth) * Cone(distance, sampleRadius) * wB;
        weight += ZCompare(sampleDepth, centerDepth) * Cone(distance, centerRadius) * wA;
        weight += 2.0f * Cylinder(distance, min(sampleRadius, centerRadius)) * max(wA, wB);
        colorDelta += (Color[samplePoint].rgb - center.rgb) * weight;
        weightSum += weight;
    }
    Result[p] = float4(center.rgb + colorDelta / weightSum, center.a);
}
