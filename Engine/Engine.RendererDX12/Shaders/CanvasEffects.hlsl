cbuffer CanvasConstants : register(b0)
{
    uint Width, Height, Seed, Stars;
    float Time;
    uint BlurRadius;
};

Texture2D<float4> Input : register(t0);
RWTexture2D<float4> Output : register(u0);
static const float Tau = 6.28318530718;

bool InBounds(uint2 pixel)
{
    return pixel.x < Width && pixel.y < Height;
}

float Random(uint key)
{
    key ^= key >> 16;
    key *= 0x7feb352du;
    key ^= key >> 15;
    key *= 0x846ca68bu;
    key ^= key >> 16;
    return float(key & 0xffffu) / 65536.0;
}

[numthreads(8, 8, 1)]
void GenerateGradient(uint3 id : SV_DispatchThreadID)
{
    if (!InBounds(id.xy)) return;
    float2 uv = (float2(id.xy) + 0.5) / float2(Width, Height);
    float phase = Tau * Random(Seed);
    float sweep = 0.5 + 0.5 * sin(Tau * (uv.x * 0.8 + uv.y * 0.4 - Time / 10.0) + phase);
    float3 cool = float3(0.024, 0.11, 0.21);
    float3 warm = float3(0.24, 0.078, 0.025);
    float3 tint = lerp(cool, warm, Random(Seed + 1));
    float stripes = 0.5 + 0.5 * cos(Tau * (uv.x * 9.0 + uv.y * 6.0 - Time / 8.0) + phase);
    float3 color = tint * (0.6 + 0.45 * sweep + 0.22 * stripes);
#if CANVAS_MEDIUM
    color += 0.026 * sin(Tau * (uv.x * 17.0 - uv.y * 11.0 + Time / 6.0) + phase);
#endif
    Output[id.xy] = float4(saturate(color), 1.0);
}

float3 Blur(uint2 pixel, int2 axis)
{
    float3 sum = 0;
    const int radius = int(BlurRadius);
    const int2 maximum = int2(Width - 1, Height - 1);
    for (int tap = -radius; tap <= radius; ++tap)
    {
        int2 source = clamp(int2(pixel) + axis * tap, int2(0, 0), maximum);
        sum += Input.Load(int3(source, 0)).rgb;
    }
    return sum / float(radius * 2 + 1);
}

[numthreads(8, 8, 1)]
void BlurHorizontal(uint3 id : SV_DispatchThreadID)
{
    if (InBounds(id.xy)) Output[id.xy] = float4(Blur(id.xy, int2(1, 0)), 1.0);
}

[numthreads(8, 8, 1)]
void FinishCanvas(uint3 id : SV_DispatchThreadID)
{
    if (!InBounds(id.xy)) return;
    float2 uv = (float2(id.xy) + 0.5) / float2(Width, Height);
    float3 color = Blur(id.xy, int2(0, 1));
    float2 bar = (uv - float2(0.12, 0.64)) / float2(0.76, 0.21);
    if (all(bar >= 0) && all(bar <= 1))
    {
        float fill = 0.60 + 0.27 * cos(Tau * (Time / 12.0 + Random(Seed)));
        color *= 0.30;
        if (bar.x < fill)
        {
            float edge = smoothstep(0.0, 0.10, min(bar.y, 1.0 - bar.y));
            color += edge * lerp(float3(0.055, 0.19, 0.22), float3(0.07, 0.48, 0.38), bar.x / fill);
            float glow = 0;
            for (uint star = 0; star < Stars; ++star)
            {
                uint key = Seed + 19u * star;
                float2 position = float2(fill * frac(Random(key) + Time / 8.0),
                    0.15 + 0.70 * Random(key + 1u));
                float2 offset = (bar - position) * float2(1.0, 0.20);
                float distanceToStar = length(offset);
                float radius = lerp(0.008, 0.020, Random(key + 2u));
                float core = 1.0 - smoothstep(radius * 0.15, radius, distanceToStar);
#if CANVAS_MEDIUM
                core += 0.35 * (1.0 - smoothstep(radius, radius * 2.5, distanceToStar));
#endif
                glow += core * (0.70 + 0.30 * sin(Tau * (Time / 4.0 + Random(key + 3u))));
            }
            color += edge * glow * float3(0.55, 0.81, 0.53);
        }
    }
    Output[id.xy] = float4(saturate(color), 1.0);
}
