#include "AOCommon.hlsli"

[numthreads(8, 8, 1)]
void NeutralCS(uint2 pixel : SV_DispatchThreadID)
{
    uint width, height;
    DestinationAO.GetDimensions(width, height);
    if (all(pixel < uint2(width, height))) DestinationAO[pixel] = 1;
}

[numthreads(8, 8, 1)]
void PrepareCS(uint2 pixel : SV_DispatchThreadID)
{
    if (any(pixel >= WorkingSize)) return;
    int2 sourcePixel = ClampPixel(pixel);
    float depth = Depth[sourcePixel];
    float3 n = mul(Normals[sourcePixel].xyz, (float3x3)CBCamera.View);
    n.z = -n.z;
    float norm = dot(n, n);
    PreparedNormals[pixel] = float4(norm > 1e-8f ? n * rsqrt(norm) : float3(0, 0, -1), 0);
    PreparedDepth[pixel] = max(depth, 1e-8f);
}

[numthreads(8, 8, 1)]
void SSAOCS(uint2 pixel : SV_DispatchThreadID)
{
    if (any(pixel >= Size)) return;
    if (Depth[pixel] <= 1e-8f) { DestinationAO[pixel] = 1; return; }
    const float3 p = ViewPosition(pixel), n = ViewNormal(pixel);
    const float phi = Hash(pixel) * 6.283185307f;
    const float3 randomVector = normalize(float3(cos(phi), sin(phi), Hash(pixel.yx + 17) * 2 - 1));
    float occlusion = 0;
    [loop]
    for (uint i = 0; i < Samples; ++i)
    {
        float z = 1 - 2 * ((i + 0.5f) / Samples);
        float angle = i * 2.39996323f;
        float r = sqrt(max(0, 1 - z * z));
        float3 offset = reflect(float3(cos(angle) * r, sin(angle) * r, z), randomVector);
        offset *= dot(offset, n) >= 0 ? 1 : -1;
        float scale = lerp(0.25f, 1, Hash(uint2(i, i + 13)));
        float3 q = p + offset * Radius * scale;
        if (q.z <= 0) continue;
        float2 uv = ProjectView(q);
        if (any(uv < 0) || any(uv >= 1)) continue;
        int2 samplePixel = min(int2(uv * Size), int2(Size) - 1);
        if (Depth[samplePixel] <= 1e-8f) continue;
        float3 v = ViewPosition(samplePixel) - p;
        float distanceToSample = length(v);
        if (distanceToSample <= 1e-5f) continue;
        float depthDifference = p.z - (p + v).z;
        float fade = saturate((Radius - depthDifference) / max(Radius * 0.8f, 1e-5f));
        if (depthDifference > Bias * Radius)
            occlusion += max(dot(n, v / distanceToSample), 0) * fade;
    }
    DestinationAO[pixel] = saturate(pow(1 - occlusion / Samples, 6.0f));
}

float3 MinDifference(float3 center, float3 next, float3 previous)
{
    float3 forward = next - center, backward = center - previous;
    return dot(forward, forward) < dot(backward, backward) ? forward : backward;
}
float TangentToSine(float tangent) { return tangent * rsqrt(1 + tangent * tangent); }

[numthreads(8, 8, 1)]
void HBAOCS(uint2 pixel : SV_DispatchThreadID)
{
    if (any(pixel >= Size)) return;
    if (Depth[pixel] <= 1e-8f) { DestinationAO[pixel] = 1; return; }
    float3 p = ViewPosition(pixel);
    float3 dx = MinDifference(p, ViewPosition(int2(pixel) + int2(1, 0)), ViewPosition(int2(pixel) - int2(1, 0)));
    float3 dy = MinDifference(p, ViewPosition(int2(pixel) + int2(0, 1)), ViewPosition(int2(pixel) - int2(0, 1)));
    float2 radiusPixels = 0.5f * float2(Projection[0][0], Projection[1][1]) * Radius / max(p.z, 1e-5f) * Size;
    if (radiusPixels.x <= 1) { DestinationAO[pixel] = 1; return; }
    uint directions = clamp(Samples / 2, 4u, 16u), steps = clamp(Samples / directions, 2u, 8u);
    float rotation = Hash(pixel) * 6.283185307f;
    float jitter = Hash(pixel + 31);
    float ao = 0;
    [loop]
    for (uint direction = 0; direction < directions; ++direction)
    {
        float angle = rotation + direction * (6.283185307f / directions);
        float2 ray = float2(cos(angle), sin(angle));
        float2 deltaPixels = ray * min(radiusPixels, 256.0f) / (steps + 1);
        float3 tangentVector = deltaPixels.x * dx + deltaPixels.y * dy;
        float tangent = -tangentVector.z / max(length(tangentVector.xy), 1e-5f) + tan(Bias * 1.5707963f);
        float sineHorizon = TangentToSine(tangent);
        [loop]
        for (uint step = 0; step < steps; ++step)
        {
            int2 samplePixel = int2(pixel) + int2(round((step + 1 + jitter) * deltaPixels));
            if (any(samplePixel < 0) || any(samplePixel >= int2(Size))) continue;
            if (Depth[samplePixel] <= 1e-8f) continue;
            float3 delta = ViewPosition(samplePixel) - p;
            float distanceSquared = dot(delta, delta);
            float sampleTangent = -delta.z / max(length(delta.xy), 1e-5f);
            if (distanceSquared < Radius * Radius && sampleTangent > tangent)
            {
                float sampleSine = TangentToSine(sampleTangent);
                ao += saturate(1 - distanceSquared / (Radius * Radius)) * (sampleSine - sineHorizon);
                tangent = sampleTangent;
                sineHorizon = sampleSine;
            }
        }
    }
    DestinationAO[pixel] = saturate(1 - ao / directions * 1.9f);
}

[numthreads(8, 8, 1)]
void BlurCS(uint2 pixel : SV_DispatchThreadID)
{
    if (any(pixel >= Size)) return;
    if (Depth[pixel] <= 1e-8f) { DestinationAO[pixel] = 1; return; }
    float3 n = ViewNormal(pixel);
    float z = ViewPosition(pixel).z;
    float value = 0, weights = 0;
    [unroll]
    for (int offset = -2; offset <= 2; ++offset)
    {
        int2 q = ClampPixel(int2(pixel) + (Horizontal != 0 ? int2(offset, 0) : int2(0, offset)));
        float spatial = exp(-0.5f * offset * offset);
        float normalWeight = pow(saturate(dot(n, ViewNormal(q))), 16);
        float depthWeight = exp(-abs(z - ViewPosition(q).z) / max(0.01f * z, Radius * 0.01f));
        float weight = spatial * normalWeight * depthWeight;
        value += SourceAO[q] * weight;
        weights += weight;
    }
    DestinationAO[pixel] = value / max(weights, 1e-6f);
}

[numthreads(8, 8, 1)]
void ResolveCS(uint2 pixel : SV_DispatchThreadID)
{
    if (any(pixel >= Size)) return;
    DestinationAO[pixel] = Depth[pixel] <= 1e-8f ? 1 : ApplyIntensity(SourceAO[pixel]);
}
