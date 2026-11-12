#ifndef LOCAL_SHADOWS_HLSLI
#define LOCAL_SHADOWS_HLSLI
float LocalVisibility(LocalLight light, float3 position, float3 normal, float nDotL)
{
    if (light.ShadowFirstSlice == 0xffffffffu || (light.ShadowFlags & 1u) == 0u) return 1;
    uint width, height, slices;
    LocalShadowMaps.GetDimensions(width, height, slices);
    float bias = light.Range / width * (0.1f + 0.2f * (1.0f - nDotL));
    float3 biasedPosition = position + normal * bias;
    uint face = 0;
    if (light.Type == 1u)
    {
        float3 offset = biasedPosition - light.Position, a = abs(offset);
        if (a.x >= a.y && a.x >= a.z) face = offset.x >= 0 ? 0 : 1;
        else if (a.y >= a.z) face = offset.y >= 0 ? 2 : 3;
        else face = offset.z >= 0 ? 4 : 5;
    }
    float4 clip = mul(float4(biasedPosition, 1), light.ShadowViewProj[face]);
    if (clip.w <= 0) return 1;
    float3 projected = clip.xyz / clip.w;
    float2 uv = projected.xy * float2(0.5f, -0.5f) + 0.5f;
    if (any(uv < 0) || any(uv > 1) || projected.z < 0 || projected.z > 1) return 1;
    float visibility = 0;
    [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x)
            visibility += LocalShadowMaps.SampleCmpLevelZero(samShadow,
                float3(clamp(uv + float2(x, y) / float2(width, height),
                    0.5f / float2(width, height), 1 - 0.5f / float2(width, height)), light.ShadowFirstSlice + face), projected.z - 0.00001f);
    return visibility / 9.0f;
}
#endif
