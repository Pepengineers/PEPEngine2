#ifndef CSM_HLSLI
#define CSM_HLSLI

float SampleCascade(uint cascade, float3 position, float3 normal, float nDotL)
{
    float normalBias = CBShadow.TexelWorldSize[cascade] * (0.25f + 0.5f * (1.0f - nDotL));
    float4 projected = mul(float4(position + normal * normalBias, 1), CBShadow.ViewProj[cascade]);
    float3 shadow = projected.xyz / projected.w;
    float2 uv = shadow.xy * float2(0.5f, -0.5f) + 0.5f;
    if (any(uv < 0.0f) || any(uv > 1.0f) || shadow.z < 0.0f || shadow.z > 1.0f) return 1.0f;
    float2 origin = float2(cascade & 1u, cascade >> 1u) * 0.5f;
    float2 atlasUV = origin + uv * 0.5f;
    float visibility = 0.0f;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
        {
            float2 sampleUV = atlasUV + float2(x, y) / 4096.0f;
            sampleUV = clamp(sampleUV, origin + 0.5f / 4096.0f, origin + 0.5f - 0.5f / 4096.0f);
            visibility += ShadowAtlas.SampleCmpLevelZero(samShadow, sampleUV, shadow.z - 0.00001f);
        }
    return visibility / 9.0f;
}

float SunVisibility(float3 position, float3 normal, float nDotL)
{
    float depth = -mul(float4(position, 1), CBCamera.View).z;
    if (depth <= 0.0f || depth >= CBShadow.SplitDepths.w) return 1.0f;
    uint cascade = depth > CBShadow.SplitDepths.x ? 1u : 0u;
    cascade += depth > CBShadow.SplitDepths.y ? 1u : 0u;
    cascade += depth > CBShadow.SplitDepths.z ? 1u : 0u;
    float visibility = SampleCascade(cascade, position, normal, nDotL);
    float previous = cascade == 0 ? CBCamera.NearPlane : CBShadow.SplitDepths[cascade - 1];
    float blendStart = lerp(previous, CBShadow.SplitDepths[cascade], 0.9f);
    float blend = saturate((depth - blendStart) / max(CBShadow.SplitDepths[cascade] - blendStart, 0.001f));
    if (blend > 0.0f)
    {
        float next = cascade < 3 ? SampleCascade(cascade + 1, position, normal, nDotL) : 1.0f;
        visibility = lerp(visibility, next, blend);
    }
    return visibility;
}

#endif
