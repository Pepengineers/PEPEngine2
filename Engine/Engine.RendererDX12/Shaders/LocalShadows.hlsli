#ifndef LOCAL_SHADOWS_HLSLI
#define LOCAL_SHADOWS_HLSLI
float LocalVisibility(LocalLight light, float3 position, float3 normal, float nDotL)
{
    if (light.ShadowFirstSlice == 0xffffffffu || (light.ShadowFlags & 1u) == 0u) return 1;
    uint width, height, slices;
    LocalShadowMaps.GetDimensions(width, height, slices);
    const bool atlas = (light.ShadowFlags & 4u) != 0u;
    const uint side = atlas ? light.ShadowTiles[0].w : width;
    float bias = light.Range / side * (0.1f + 0.2f * (1.0f - nDotL));
    float3 biasedPosition = position + normal * bias;
    uint face = 0;
    if (light.Type == 1u)
    {
        float3 offset = biasedPosition - light.Position, a = abs(offset);
        if (a.x >= a.y && a.x >= a.z) face = offset.x >= 0 ? 0 : 1;
        else if (a.y >= a.z) face = offset.y >= 0 ? 2 : 3;
        else face = offset.z >= 0 ? 4 : 5;
    }
    // Meta opts into a per-face mask; other paths retain their existing full maps.
    if ((light.ShadowFlags & 4u) && (light._padding.z & (1u << face)) == 0u) return 1;
    float4 clip = mul(float4(biasedPosition, 1), light.ShadowViewProj[face]);
    if (clip.w <= 0) return 1;
    float3 projected = clip.xyz / clip.w;
    float2 uv = projected.xy * float2(0.5f, -0.5f) + 0.5f;
    if (any(uv < 0) || any(uv > 1) || projected.z < 0 || projected.z > 1) return 1;
    const uint4 tile = atlas ? light.ShadowTiles[face] : uint4(light.ShadowFirstSlice + face, 0, 0, width);
    const float2 origin = float2(tile.yz);
    const float2 texel = origin + uv * (atlas ? float2(tile.ww) : float2(width, height));
    const float2 lower = origin + 0.5f;
    const float2 upper = origin + (atlas ? float2(tile.ww) : float2(width, height)) - 0.5f;
    float visibility = 0;
    [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x)
            visibility += LocalShadowMaps.SampleCmpLevelZero(samShadow,
                float3(clamp(texel + float2(x, y), lower, upper) / float2(width, height), tile.x), projected.z - 0.00001f);
    return visibility / 9.0f;
}
#endif
