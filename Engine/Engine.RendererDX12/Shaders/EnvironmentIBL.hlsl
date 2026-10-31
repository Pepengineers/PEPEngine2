#include "PBRMath.hlsli"

cbuffer FilterParameters : register(b0) { uint OutputSize; float Roughness; };
TextureCube<float4> SourceEnvironment : register(t0);
SamplerState EnvironmentSampler : register(s0);
RWTexture2DArray<float4> OutputCube : register(u0);
RWTexture2D<float4> OutputLUT : register(u1);

float2 Hammersley(uint index, uint count)
{
    return float2(float(index) / count, reversebits(index) * 2.3283064365386963e-10f);
}

float3 CubeDirection(uint3 id)
{
    float2 uv = (float2(id.xy) + 0.5f) / OutputSize * 2.0f - 1.0f;
    float3 direction;
    if (id.z == 0) direction = float3(1, -uv.y, -uv.x);
    else if (id.z == 1) direction = float3(-1, -uv.y, uv.x);
    else if (id.z == 2) direction = float3(uv.x, 1, uv.y);
    else if (id.z == 3) direction = float3(uv.x, -1, -uv.y);
    else if (id.z == 4) direction = float3(uv.x, -uv.y, 1);
    else direction = float3(-uv.x, -uv.y, -1);
    return normalize(direction);
}

float3 ToWorld(float3 local, float3 normal)
{
    float3 up = abs(normal.z) < 0.999f ? float3(0, 0, 1) : float3(1, 0, 0);
    float3 tangent = normalize(cross(up, normal));
    return tangent * local.x + cross(normal, tangent) * local.y + normal * local.z;
}

float3 SampleGGX(float2 xi, float3 normal, float roughness)
{
    float cosTheta = GGXSampleCosTheta(xi.y, roughness);
    float sinTheta = sqrt(saturate(1.0f - cosTheta * cosTheta));
    float phi = 2.0f * PBR_PI * xi.x;
    return ToWorld(float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta), normal);
}

[numthreads(8, 8, 1)]
void IrradianceCS(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= OutputSize)) return;
    float3 n = CubeDirection(id);
    float3 irradiance = 0.0f;
    uint width, height, levels;
    SourceEnvironment.GetDimensions(0, width, height, levels);
    for (uint i = 0; i < 128; ++i)
    {
        float2 xi = Hammersley(i, 128);
        float phi = 2.0f * PBR_PI * xi.x;
        float3 local = float3(cos(phi) * sqrt(xi.y), sin(phi) * sqrt(xi.y), sqrt(1.0f - xi.y));
        float mip = max(0.0f, 0.5f * log2(6.0f * width * height / (512.0f * local.z)));
        irradiance += SourceEnvironment.SampleLevel(EnvironmentSampler, ToWorld(local, n), mip).rgb;
    }
    OutputCube[id] = float4(irradiance * (PBR_PI / 128.0f), 1);
}

[numthreads(8, 8, 1)]
void PrefilterCS(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= OutputSize)) return;
    float3 n = CubeDirection(id);
    if (Roughness == 0.0f)
    {
        OutputCube[id] = float4(SourceEnvironment.SampleLevel(EnvironmentSampler, n, 0).rgb, 1);
        return;
    }
    float3 color = 0.0f;
    float totalWeight = 0.0f;
    uint width, height, levels;
    SourceEnvironment.GetDimensions(0, width, height, levels);
    float texelSolidAngle = 4.0f * PBR_PI / (6.0f * width * height);
    for (uint i = 0; i < 128; ++i)
    {
        float3 h = SampleGGX(Hammersley(i, 128), n, Roughness);
        float3 l = reflect(-n, h);
        float nDotL = saturate(dot(n, l));
        float pdf = max(DistributionGGX(saturate(dot(n, h)), Roughness) * 0.25f, 1e-5f);
        float mip = max(0.0f, 0.5f * log2(1.0f / (128.0f * pdf * texelSolidAngle)));
        color += SourceEnvironment.SampleLevel(EnvironmentSampler, l, mip).rgb * nDotL;
        totalWeight += nDotL;
    }
    OutputCube[id] = float4(color / max(totalWeight, 1e-5f), 1);
}

[numthreads(8, 8, 1)]
void IntegrateBRDFCS(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= OutputSize)) return;
    float2 uv = (float2(id.xy) + 0.5f) / OutputSize;
    float nDotV = uv.x;
    float roughness = max(0.045f, uv.y);
    float3 v = float3(sqrt(1.0f - nDotV * nDotV), 0, nDotV);
    float2 integrated = 0.0f;
    for (uint i = 0; i < 256; ++i)
    {
        float3 h = SampleGGX(Hammersley(i, 256), float3(0, 0, 1), roughness);
        float3 l = reflect(-v, h);
        float nDotL = saturate(l.z);
        float vDotH = saturate(dot(v, h));
        if (nDotL <= 0.0f) continue;
        float visibility = SplitSumVisibility(nDotV, nDotL, h.z, vDotH, roughness);
        float fresnel = FresnelSchlickFactor(vDotH);
        integrated += float2(1.0f - fresnel, fresnel) * visibility;
    }
    OutputLUT[id.xy] = float4(integrated / 256.0f, 0, 1);
}
