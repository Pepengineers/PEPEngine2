#include "CBufferStructures.hlsl"
#define XE_GTAO_USE_HALF_FLOAT_PRECISION 0
#define XE_GTAO_FP32_DEPTHS
#define XE_GTAO_USE_DEFAULT_CONSTANTS 0
#define VA_SATURATE saturate
#include "../../../External/External.Libraries/XeGTAO/XeGTAO.hlsli"

cbuffer DispatchParameters : register(b0)
{
    uint BlurAxis;
    uint FinalDenoise;
    float XeScale;
    uint Unused;
};
ConstantBuffer<CameraCB> Camera : register(b1);
cbuffer AOParameters : register(b2)
{
    float RadiusCm, Intensity, BiasCm, HorizonBias;
    float FadeStartCm, FadeEndCm, Sharpness;
    uint Samples;
    uint Directions, Steps, DenoisePasses, Implementation;
    float Slices, SliceSteps;
    float2 Padding;
};
ConstantBuffer<GTAOConstants> Xe : register(b3);
Texture2D<float> Depth : register(t0);
Texture2D<float4> Normals : register(t1);
Texture2D<float> InputAO : register(t2);
Texture2D<uint> XeInput : register(t3);
Texture2D<float> Edges : register(t4);
RWTexture2D<float> OutputAO : register(u0);
RWTexture2D<float4> PreparedNormals : register(u1);
RWTexture2D<float> Depth0 : register(u2);
RWTexture2D<float> Depth1 : register(u3);
RWTexture2D<float> Depth2 : register(u4);
RWTexture2D<float> Depth3 : register(u5);
RWTexture2D<float> Depth4 : register(u6);
RWTexture2D<uint> XeOutput : register(u7);
RWTexture2D<unorm float> OutputEdges : register(u8);
SamplerState PointClamp : register(s0);

[numthreads(8, 8, 1)]
void NeutralCS(uint3 id : SV_DispatchThreadID)
{
    uint w, h;
    OutputAO.GetDimensions(w, h);
    if (all(id.xy < uint2(w, h))) OutputAO[id.xy] = 1;
}

bool InBounds(uint2 p)
{
    uint w, h;
    Depth.GetDimensions(w, h);
    return all(p < uint2(w, h));
}

float LinearDepth(float d) { return XeGTAO_ScreenSpaceToViewSpaceDepth(d, Xe); }
float3 Position(uint2 p)
{
    return XeGTAO_ComputeViewspacePosition((p + 0.5f) * Xe.ViewportPixelSize, LinearDepth(Depth[p]), Xe);
}
float Noise(uint2 p) { return frac(52.9829189f * frac(dot(float2(p), float2(0.06711056f, 0.00583715f)))); }

[numthreads(8, 8, 1)]
void PrepareNormalsCS(uint3 id : SV_DispatchThreadID)
{
    if (!InBounds(id.xy)) return;
    float3 normal = mul(Normals[id.xy].xyz, (float3x3)Camera.View);
    normal = dot(normal, normal) > 1e-8f ? normalize(normal) : float3(0, 0, 1);
    // Both official libraries reconstruct positive-Z view positions.
    normal.z = -normal.z;
    PreparedNormals[id.xy] = float4(normal * 0.5f + 0.5f, 1);
}

[numthreads(8, 8, 1)]
void SSAOCS(uint3 id : SV_DispatchThreadID)
{
    uint2 p = id.xy;
    if (!InBounds(p)) return;
    if (Depth[p] <= 0) { OutputAO[p] = 1; return; }
    float3 center = Position(p), normal = Normals[p].xyz * 2 - 1;
    float3 tangent = normalize(cross(abs(normal.z) < 0.99f ? float3(0, 0, 1) : float3(0, 1, 0), normal));
    float3 bitangent = cross(normal, tangent);
    float rotation = Noise(p) * 2 * XE_GTAO_PI;
    float occlusion = 0;
    for (uint i = 0; i < Samples; ++i)
    {
        float t = (i + 0.5f) / Samples;
        float angle = i * 2.39996323f + rotation;
        float3 direction = tangent * (cos(angle) * sqrt(t)) + bitangent * (sin(angle) * sqrt(t)) + normal * sqrt(1 - t);
        float3 samplePosition = center + direction * RadiusCm * lerp(0.1f, 1.0f, t * t);
        if (samplePosition.z <= 0) continue;
        float2 uv = (samplePosition.xy / samplePosition.z - Xe.NDCToViewAdd) / Xe.NDCToViewMul;
        if (any(uv < 0) || any(uv >= 1)) continue;
        float raw = Depth.SampleLevel(PointClamp, uv, 0);
        if (raw <= 0) continue;
        float z = LinearDepth(raw);
        float weight = saturate(1 - abs(z - center.z) / RadiusCm);
        occlusion += (z < samplePosition.z - BiasCm ? weight : 0);
    }
    OutputAO[p] = saturate(1 - occlusion / Samples);
}

[numthreads(8, 8, 1)]
void HBAOCS(uint3 id : SV_DispatchThreadID)
{
    uint2 p = id.xy;
    if (!InBounds(p)) return;
    if (Depth[p] <= 0) { OutputAO[p] = 1; return; }
    float3 center = Position(p), normal = Normals[p].xyz * 2 - 1;
    float radiusPixels = RadiusCm / max(center.z * abs(Xe.NDCToViewMul_x_PixelSize.y), 1e-5f);
    float rotation = Noise(p) * 2 * XE_GTAO_PI;
    float occlusion = 0;
    for (uint direction = 0; direction < Directions; ++direction)
    {
        float angle = rotation + direction * (2 * XE_GTAO_PI / Directions);
        float2 ray = float2(cos(angle), sin(angle));
        float horizon = HorizonBias;
        for (uint step = 0; step < Steps; ++step)
        {
            float distancePixels = max(1, radiusPixels * (step + 0.5f) / Steps);
            int2 q = int2(round(float2(p) + ray * distancePixels));
            if (any(q < 0) || !InBounds(q) || Depth[q] <= 0) continue;
            float3 delta = Position(q) - center;
            float distanceSquared = dot(delta, delta);
            if (distanceSquared < 1e-6f || distanceSquared >= RadiusCm * RadiusCm) continue;
            float sine = dot(normal, delta) * rsqrt(distanceSquared);
            if (sine > horizon)
            {
                occlusion += (sine - horizon) * saturate(1 - distanceSquared / (RadiusCm * RadiusCm));
                horizon = sine;
            }
        }
    }
    OutputAO[p] = saturate(1 - occlusion / (Directions * (1 - HorizonBias)));
}

[numthreads(8, 8, 1)]
void BlurCS(uint3 id : SV_DispatchThreadID)
{
    uint2 p = id.xy;
    if (!InBounds(p)) return;
    if (Depth[p] <= 0) { OutputAO[p] = 1; return; }
    float centerDepth = LinearDepth(Depth[p]);
    float3 normal = Normals[p].xyz * 2 - 1;
    float sum = InputAO[p], weightSum = 1;
    for (int offset = -2; offset <= 2; ++offset)
    {
        if (offset == 0) continue;
        int2 q = int2(p) + (BlurAxis == 0 ? int2(offset, 0) : int2(0, offset));
        if (any(q < 0) || !InBounds(q) || Depth[q] <= 0) continue;
        float depthDelta = abs(LinearDepth(Depth[q]) - centerDepth);
        float3 otherNormal = Normals[q].xyz * 2 - 1;
        float weight = exp2(-offset * offset * 0.5f - depthDelta * lerp(0.01f, 4.0f, Sharpness) / max(RadiusCm * 0.02f, 0.1f));
        weight *= pow(saturate(dot(normal, otherNormal)), 8);
        sum += InputAO[q] * weight;
        weightSum += weight;
    }
    OutputAO[p] = sum / weightSum;
}

[numthreads(8, 8, 1)]
void ResolveCS(uint3 id : SV_DispatchThreadID)
{
    uint2 p = id.xy;
    if (!InBounds(p)) return;
    if (Depth[p] <= 0 || LinearDepth(Depth[p]) >= FadeEndCm)
    {
        OutputAO[p] = 1;
        return;
    }
    float visibility = Implementation == 2 ? XeInput[p] * (XeScale / 255.0f) : InputAO[p];
    if (Implementation != 3)
    {
        float fade = saturate((FadeEndCm - LinearDepth(Depth[p])) / (FadeEndCm - FadeStartCm));
        visibility = 1 - (1 - saturate(visibility)) * Intensity * fade;
    }
    OutputAO[p] = saturate(visibility);
}

[numthreads(8, 8, 1)]
void XePrefilterCS(uint3 id : SV_DispatchThreadID, uint3 thread : SV_GroupThreadID)
{
    XeGTAO_PrefilterDepths16x16(id.xy, thread.xy, Xe, Depth, PointClamp, Depth0, Depth1, Depth2, Depth3, Depth4);
}

[numthreads(8, 8, 1)]
void XeMainCS(uint3 id : SV_DispatchThreadID)
{
    if (!InBounds(id.xy)) return;
    GTAOConstants constants = Xe;
    uint w, h;
    InputAO.GetDimensions(w, h);
    // Keep the official five-mip path valid even for viewports smaller than 16 pixels.
    constants.NDCToViewMul *= float2(w, h) / float2(Xe.ViewportSize);
    constants.ViewportSize = int2(w, h);
    constants.ViewportPixelSize = 1.0f / float2(w, h);
    uint index = HilbertIndex(id.x, id.y) + 288 * (Xe.NoiseIndex % 64);
    float2 noise = frac(0.5f + index * float2(0.7548776662f, 0.5698402910f));
    XeGTAO_MainPass(id.xy, Slices, SliceSteps, noise, Normals[id.xy].xyz * 2 - 1,
        constants, InputAO, PointClamp, XeOutput, OutputEdges);
}

[numthreads(8, 8, 1)]
void XeDenoiseCS(uint3 id : SV_DispatchThreadID)
{
    if (!InBounds(uint2(id.x * 2, id.y))) return;
    XeGTAO_Denoise(uint2(id.x * 2, id.y), Xe, XeInput, Edges, PointClamp, XeOutput, FinalDenoise != 0);
}
