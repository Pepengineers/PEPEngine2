// Guertin/McGuire/Nowrouzezahrai, HPG 2014, sections 4-5.
// Use section 4.1's normalized second direction; Appendix A's d=vc/vmax is ambiguous.
cbuffer Parameters : register(b0)
{
    float4x4 InverseViewProjection, PreviousViewProjection;
    uint2 Size, TileCount;
    uint TileSize, Samples;
    float HalfShutter, MaxRadius;
    float2 DepthUnpack, Padding;
    float4 CameraLocation;
};
Texture2D<float4> Color : register(t0);
Texture2D<float> Depth : register(t1);
Texture2D<float2> Motion : register(t2);
Texture2D<float2> TileVelocities : register(t3);
Texture2D<float2> NeighborVelocities : register(t4);
RWTexture2D<float4> Result : register(u0);
RWTexture2D<float2> TileResult : register(u1);
RWTexture2D<float2> NeighborResult : register(u2);
SamplerState LinearClamp : register(s0);

float2 Velocity(uint2 pixel)
{
    float2 motion = Motion[pixel];
    if (Depth[pixel] <= 0)
    {
        float2 ndc = (float2(pixel) + 0.5) / Size * float2(2, -2) + float2(-1, 1);
        float4 world = mul(float4(ndc, 0, 1), InverseViewProjection);
        float3 ray = world.xyz - CameraLocation.xyz * world.w;
        float4 previous = mul(float4(ray, 0), PreviousViewProjection);
        motion = previous.w > 1e-6 ? previous.xy / previous.w - ndc : 0;
    }
    float2 velocity = motion * float2(0.5, -0.5) * Size * HalfShutter;
    return velocity * min(1, MaxRadius / max(length(velocity), 1e-6));
}
groupshared float2 TileReduction[256];
[numthreads(16, 16, 1)]
void TileMaxCS(uint3 group : SV_GroupID, uint lane : SV_GroupIndex)
{
    float2 best = 0;
    for (uint i = lane; i < TileSize * TileSize; i += 256)
    {
        uint2 p = group.xy * TileSize + uint2(i % TileSize, i / TileSize);
        if (all(p < Size))
        {
            float2 value = Velocity(p);
            if (dot(value, value) > dot(best, best)) best = value;
        }
    }
    TileReduction[lane] = best;
    GroupMemoryBarrierWithGroupSync();
    for (uint stride = 128; stride > 0; stride >>= 1)
    {
        if (lane < stride)
        {
            float2 other = TileReduction[lane + stride];
            if (dot(other, other) > dot(TileReduction[lane], TileReduction[lane])) TileReduction[lane] = other;
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (lane == 0) TileResult[group.xy] = TileReduction[0];
}
[numthreads(8, 8, 1)]
void NeighborMaxCS(uint3 dispatch : SV_DispatchThreadID)
{
    int2 tile = int2(dispatch.xy);
    if (any(dispatch.xy >= TileCount)) return;
    float2 best = 0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
        {
            int2 p = tile + int2(x, y);
            if (any(p < 0) || any(p >= int2(TileCount))) continue;
            float2 value = TileVelocities[p];
            // A centered diagonal segment must cross both shared tile boundaries.
            if (x != 0 && y != 0 && (x * value.x) * (y * value.y) <= 0) continue;
            if (dot(value, value) > dot(best, best)) best = value;
        }
    NeighborResult[tile] = best;
}
float Halton(uint index, uint base)
{
    float value = 0, fraction = 1;
    while (index > 0)
    {
        fraction /= base;
        value += fraction * (index % base);
        index /= base;
    }
    return value;
}
float ReciprocalDepth(uint2 pixel)
{
    float depth = Depth[pixel];
    return depth > 0 ? max(0, (depth + DepthUnpack.x) / DepthUnpack.y) : 0;
}
float CompareDepth(float a, float b)
{
    if (a == b) return 1;
    return saturate(1 - (a - b) / max(min(a, b), 1e-6));
}
float2 NeighborVelocity(uint2 pixel)
{
    int2 tile = int2(pixel / TileSize);
    float2 fraction = (float2(pixel % TileSize) + 0.5) / TileSize;
    float2 distance = min(fraction, 1 - fraction);
    float jitter = Halton(pixel.x + pixel.y * Size.x + 1, 3);
    // Jitter only an axial tile neighbour; interpolating directions cancels opposite motion.
    if (min(distance.x, distance.y) < 0.25 && jitter < 1 - 4 * min(distance.x, distance.y))
    {
        if (distance.x < distance.y) tile.x += fraction.x < 0.5 ? -1 : 1;
        else tile.y += fraction.y < 0.5 ? -1 : 1;
    }
    tile = clamp(tile, int2(0, 0), int2(TileCount) - 1);
    return NeighborVelocities[tile];
}
[numthreads(8, 8, 1)]
void GatherCS(uint3 dispatch : SV_DispatchThreadID)
{
    uint2 pixel = dispatch.xy;
    if (any(pixel >= Size)) return;
    float4 center = Color[pixel];
    float2 dominantVelocity = NeighborVelocity(pixel);
    float radius = length(dominantVelocity);
    if (radius <= 0.5) { Result[pixel] = center; return; }
    float2 localVelocity = Velocity(pixel);
    float localSpeed = length(localVelocity);
    float2 dominant = dominantVelocity / radius;
    float2 perpendicular = float2(-dominant.y, dominant.x);
    if (dot(perpendicular, localVelocity) < 0) perpendicular = -perpendicular;
    float2 localDirection = localSpeed > 1e-6 ? localVelocity / localSpeed : perpendicular;
    float2 blended = lerp(perpendicular, localDirection, saturate((localSpeed - 0.5) / 1.5));
    float2 centerDirection = blended / max(length(blended), 1e-6);
    float centerDepth = ReciprocalDepth(pixel);
    float total = Samples / (40.0 * max(localSpeed, 0.5));
    float4 sum = center * total;
    float jitter = 0;
    for (uint i = 0; i < Samples; ++i)
    {
        uint directionIndex = i & 1;
        uint count = (Samples + 1 - directionIndex) / 2;
        uint tap = i / 2;
        uint pairs = count / 2;
        float2 direction = directionIndex == 0 ? centerDirection : dominant;
        if ((i & 3) == 0)
            jitter = (Halton(pixel.x + pixel.y * Size.x + tap / 2 + 1, 2) - 0.5) * min(1.0, 0.95 * 27.0 / Samples);
        // Mirrored jitter pairs keep the centered exposure invariant to velocity sign.
        float t = tap < 2 * pairs ? ((tap / 2 + 0.5 + jitter) / max(pairs, 1)) * ((tap & 1) ? -1 : 1) : 0;
        float offset = t * radius;
        float2 position = float2(pixel) + direction * offset;
        if (any(position < 0) || any(position > float2(Size - 1))) continue;
        uint2 samplePixel = uint2(position + 0.5);
        float2 sampleVelocity = Velocity(samplePixel);
        float sampleSpeed = length(sampleVelocity);
        float a = abs(dot(centerDirection, direction));
        float b = abs(dot(sampleVelocity / max(sampleSpeed, 0.5), direction));
        float sampleDepth = ReciprocalDepth(samplePixel);
        float front = CompareDepth(centerDepth, sampleDepth);
        float back = CompareDepth(sampleDepth, centerDepth);
        float overlap = max(min(sampleSpeed, localSpeed), 0.5);
        float weight = front * saturate(1 - abs(offset) / max(sampleSpeed, 0.5)) * b
            + back * saturate(1 - abs(offset) / max(localSpeed, 0.5)) * a
            + 2 * (1 - smoothstep(0.95 * overlap, 1.05 * overlap, abs(offset))) * max(a, b);
        sum += Color.SampleLevel(LinearClamp, (position + 0.5) / Size, 0) * weight;
        total += weight;
    }
    Result[pixel] = sum / total;
}
