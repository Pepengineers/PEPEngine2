// Four fixed reads, no per-consumer source loop. Zero outside the local field.
float4 SampleInteraction(StructuredBuffer<float4> field, float2 position, float4 area, uint resolution)
{
    float2 uv = (position - area.xy) / (2 * area.zw) + 0.5f;
    if (resolution == 0 || any(uv < 0) || any(uv > 1)) return 0;
    float2 p = clamp(uv * resolution - 0.5f, 0, float(resolution - 1));
    uint2 lo = uint2(p), hi = min(lo + 1, resolution - 1);
    float2 f = frac(p);
    return lerp(lerp(field[lo.y * resolution + lo.x], field[lo.y * resolution + hi.x], f.x),
        lerp(field[hi.y * resolution + lo.x], field[hi.y * resolution + hi.x], f.x), f.y);
}
