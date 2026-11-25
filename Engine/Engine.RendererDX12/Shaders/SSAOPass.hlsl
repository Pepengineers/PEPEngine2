#include "CBufferStructures.hlsli"

cbuffer Parameters : register(b0)
{
    float Radius, Intensity, Bias, Sharpness;
    uint Samples, BlurAxis;
    float FadeStart, FadeEnd;
};
ConstantBuffer<CameraCB> Camera : register(b1);
Texture2D<float> Depth : register(t0);
Texture2D<float4> Normals : register(t1);
Texture2D<float> InputAO : register(t2);
RWTexture2D<float> OutputAO : register(u0);
SamplerState samPointClamp : register(s1);

float LinearDepth(float depth)
{ return Camera.NearPlane * Camera.FarPlane / (Camera.NearPlane + depth * (Camera.FarPlane - Camera.NearPlane)); }

float3 WorldPosition(uint2 pixel, uint2 size, float depth)
{
    float2 ndc = (float2(pixel) + 0.5f) / size * float2(2, -2) + float2(-1, 1);
    float4 world = mul(float4(ndc, depth, 1), Camera.InvViewProj);
    return world.xyz / world.w;
}

// Hemisphere sampling and depth-range weighting, expressed in world space
[numthreads(8, 8, 1)]
void SSAOCS(uint3 id : SV_DispatchThreadID)
{
    uint2 size, pixel = id.xy;
    Depth.GetDimensions(size.x, size.y);
    if (any(pixel >= size)) return;
    float depth = Depth[pixel];
    if (depth <= 0 || Intensity == 0) { OutputAO[pixel] = 1; return; }
    float3 center = WorldPosition(pixel, size, depth);
    float centerDepth = LinearDepth(depth);
    float3 normal = SafeNormalize(Normals[pixel].xyz);
    float3 tangent = normalize(cross(abs(normal.z) < 0.99f ? float3(0, 0, 1) : float3(0, 1, 0), normal));
    float3 bitangent = cross(normal, tangent);
    float noise = frac(52.9829189f * frac(dot(float2(pixel), float2(0.06711056f, 0.00583715f))));
    float rotation = noise * 6.28318530718f;
    float occlusion = 0;
    for (uint i = 0; i < Samples; ++i)
    {
        float t = (i + 0.5f) / Samples;
        float angle = i * 2.39996323f + rotation;
        float3 direction = tangent * (cos(angle) * sqrt(t)) + bitangent * (sin(angle) * sqrt(t)) + normal * sqrt(1 - t);
        float3 samplePosition = center + direction * Radius * lerp(0.1f, 1.0f, t * t);
        float4 clip = mul(float4(samplePosition, 1), Camera.ViewProj);
        if (clip.w <= 0) continue;
        float2 uv = clip.xy / clip.w * float2(0.5f, -0.5f) + 0.5f;
        if (any(uv < 0) || any(uv >= 1)) continue;
        float raw = Depth.SampleLevel(samPointClamp, uv, 0);
        if (raw <= 0) continue;
        float z = LinearDepth(raw);
        float weight = saturate(1 - abs(z - centerDepth) / Radius);
        occlusion += z < clip.w - Bias ? weight : 0;
    }
    OutputAO[pixel] = saturate(1 - occlusion / Samples);
}

// Separable depth/normal-aware blur
[numthreads(8, 8, 1)]
void BlurCS(uint3 id : SV_DispatchThreadID)
{
    uint2 size, pixel = id.xy;
    Depth.GetDimensions(size.x, size.y);
    if (any(pixel >= size)) return;
    if (Depth[pixel] <= 0 || Intensity == 0) { OutputAO[pixel] = 1; return; }
    float centerDepth = LinearDepth(Depth[pixel]);
    float3 normal = SafeNormalize(Normals[pixel].xyz);
    float sum = InputAO[pixel], weightSum = 1;
    for (int offset = -2; offset <= 2; ++offset)
    {
        if (offset == 0) continue;
        int2 q = int2(pixel) + (BlurAxis == 0 ? int2(offset, 0) : int2(0, offset));
        if (any(q < 0) || any(q >= int2(size)) || Depth[q] <= 0) continue;
        float depthDelta = abs(LinearDepth(Depth[q]) - centerDepth);
        float weight = exp2(-offset * offset * 0.5f - depthDelta * lerp(0.01f, 4.0f, Sharpness) / max(Radius * 0.02f, 0.1f));
        weight *= pow(saturate(dot(normal, SafeNormalize(Normals[q].xyz))), 8);
        sum += InputAO[q] * weight;
        weightSum += weight;
    }
    float visibility = sum / weightSum;
    if (BlurAxis != 0)
    {
        float fade = saturate((FadeEnd - centerDepth) / (FadeEnd - FadeStart));
        visibility = saturate(1 - (1 - visibility) * Intensity * fade);
    }
    OutputAO[pixel] = visibility;
}
