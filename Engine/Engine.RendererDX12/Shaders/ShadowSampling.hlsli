#ifndef SHADOW_SAMPLING_HLSLI
#define SHADOW_SAMPLING_HLSLI

#include "CBufferStructures.hlsli"

float SampleShadow(Texture2DArray<float> maps, SamplerComparisonState shadowSampler,
    ShadowView view, float3 position, float3 normal, float normalBias)
{
    float4 clip = mul(float4(position + normal * normalBias, 1), view.ViewProj);
    if (clip.w <= 0) return 1;
    float3 projected = clip.xyz / clip.w;
    float2 uv = projected.xy * float2(0.5f, -0.5f) + 0.5f;
    if (any(uv < 0) || any(uv > 1) || projected.z < 0 || projected.z > 1) return 1;

    uint width, height, layers;
    maps.GetDimensions(width, height, layers);
    float2 texelSize = 1.0f / float2(width, height);
    float visibility = 0;
    [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x)
        {
            float2 sampleUV = clamp(uv + float2(x, y) * texelSize, 0.5f * texelSize, 1 - 0.5f * texelSize);
            visibility += maps.SampleCmpLevelZero(shadowSampler, float3(sampleUV, view.Layer), projected.z - 0.00001f);
        }
    return visibility / 9.0f;
}

float SampleCascade(Texture2DArray<float> directionalMaps, StructuredBuffer<ShadowView> shadowViews,
    SamplerComparisonState shadowSampler, uint viewIndex, float3 position, float3 normal, float nDotL)
{
    ShadowView view = shadowViews[viewIndex];
    float normalBias = view.NormalBias * (0.25f + 0.5f * (1 - nDotL));
    return SampleShadow(directionalMaps, shadowSampler, view, position, normal, normalBias);
}

float ShadowVisibility(Texture2DArray<float> directionalMaps, Texture2DArray<float> pointSpotMaps,
    StructuredBuffer<ShadowView> shadowViews, SamplerComparisonState shadowSampler,
    GDX12LightConstants light, float3 position, float3 normal, float nDotL, float viewDepth, float nearPlane)
{
    uint first = light.FirstShadowViewIndex;
    if (first == NO_SHADOW_VIEW) return 1;
    if (light.Type == LIGHT_TYPE_DIRECTIONAL)
    {
        if (viewDepth <= 0 || viewDepth >= shadowViews[first + 3].SplitDepth) return 1;
        uint cascade = viewDepth > shadowViews[first].SplitDepth ? 1u : 0u;
        cascade += viewDepth > shadowViews[first + 1].SplitDepth ? 1u : 0u;
        cascade += viewDepth > shadowViews[first + 2].SplitDepth ? 1u : 0u;
        float visibility = SampleCascade(directionalMaps, shadowViews, shadowSampler, first + cascade, position, normal, nDotL);
        float previous = cascade == 0 ? nearPlane : shadowViews[first + cascade - 1].SplitDepth;
        float split = shadowViews[first + cascade].SplitDepth;
        float blendStart = lerp(previous, split, 0.9f);
        float blend = saturate((viewDepth - blendStart) / max(split - blendStart, 0.001f));
        if (blend > 0)
        {
            float next = cascade < 3 ? SampleCascade(directionalMaps, shadowViews, shadowSampler, first + cascade + 1, position, normal, nDotL) : 1;
            visibility = lerp(visibility, next, blend);
        }
        return visibility;
    }

    float normalBias = shadowViews[first].NormalBias * (0.1f + 0.2f * (1 - nDotL));
    uint face = 0;
    if (light.Type == LIGHT_TYPE_POINT)
    {
        float3 offset = position + normal * normalBias - light.Position;
        float3 a = abs(offset);
        if (a.x >= a.y && a.x >= a.z) face = offset.x >= 0 ? 0u : 1u;
        else if (a.y >= a.z) face = offset.y >= 0 ? 2u : 3u;
        else face = offset.z >= 0 ? 4u : 5u;
    }
    return SampleShadow(pointSpotMaps, shadowSampler, shadowViews[first + face], position, normal, normalBias);
}

#endif
