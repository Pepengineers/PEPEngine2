float2 SampleWindGrid(StructuredBuffer<float4> field, float2 uv, uint resolution)
{
    float2 p = clamp(uv * resolution - 0.5f, 0, float(resolution - 1));
    uint2 low = uint2(floor(p));
    uint2 high = min(low + 1, resolution - 1);
    float2 f = frac(p);
    return lerp(lerp(field[low.y * resolution + low.x].xy, field[low.y * resolution + high.x].xy, f.x),
                lerp(field[high.y * resolution + low.x].xy, field[high.y * resolution + high.x].xy, f.x), f.y);
}
