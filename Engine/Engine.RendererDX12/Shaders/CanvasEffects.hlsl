cbuffer CanvasConstants : register(b0)
{
    uint Width, Height, Seed, Stars;
    float Time;
    uint BlurRadius, Style;
};

Texture2D<float4> Input : register(t0);
Texture2D<float4> Blurred : register(t1);
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
    float grain = Random(id.x + id.y * Width + 937u * Seed);
    float3 color;
    if (Style == 1)
    {
        float mortar = step(0.93, frac(uv.y * 9.0));
        float vertical = step(0.96, frac(uv.x * 8.0 + floor(uv.y * 9.0) * 0.5));
        color = float3(0.11, 0.10, 0.15) * (0.75 + grain * 0.40);
        color *= 1.0 - 0.40 * max(mortar, vertical);
        color += float3(0.17, 0.01, 0.07) * exp(-18.0 * dot(uv - 0.5, uv - 0.5));
    }
    else
    {
        float sweep = 0.5 + 0.5 * sin(Tau * (uv.x * 0.6 - uv.y * 0.3 + Time * 0.07));
        float2 grid = abs(frac(uv * float2(24, 14) + float2(Time * 0.04, 0)) - 0.5);
        float gridLine = step(0.47, max(grid.x, grid.y));
        float scan = exp(-160.0 * abs(uv.y - frac(Time * 0.12)));
        color = float3(0.008, 0.012, 0.04) + lerp(float3(0.02, 0.16, 0.18), float3(0.18, 0.005, 0.12), sweep) * 0.42;
        color += gridLine * float3(0.005, 0.035, 0.055) + scan * float3(0.10, 0.35, 0.32);
        color += grain * 0.01;
    }
#if CANVAS_MEDIUM
    color += 0.008 * sin(Tau * (uv.x * 28.0 - uv.y * 13.0 + Time * 0.15));
#endif
    Output[id.xy] = float4(saturate(color), 1.0);
}

float4 Gaussian(uint2 pixel, int2 axis)
{
    float4 sum = 0;
    float weightSum = 0;
    const int radius = int(BlurRadius);
    const float sigma = max(float(radius) * 0.45, 0.5);
    const int2 maximum = int2(Width - 1, Height - 1);
    for (int tap = -radius; tap <= radius; ++tap)
    {
        const float weight = exp(-float(tap * tap) / (2.0 * sigma * sigma));
        int2 source = clamp(int2(pixel) + axis * tap, int2(0, 0), maximum);
        sum += Input.Load(int3(source, 0)) * weight;
        weightSum += weight;
    }
    return sum / weightSum;
}

[numthreads(8, 8, 1)]
void BlurHorizontal(uint3 id : SV_DispatchThreadID)
{
    if (InBounds(id.xy)) Output[id.xy] = Gaussian(id.xy, int2(1, 0));
}

[numthreads(8, 8, 1)]
void GaussianVertical(uint3 id : SV_DispatchThreadID)
{
    if (InBounds(id.xy)) Output[id.xy] = Gaussian(id.xy, int2(0, 1));
}

[numthreads(8, 8, 1)]
void FinishCanvas(uint3 id : SV_DispatchThreadID)
{
    if (InBounds(id.xy)) Output[id.xy] = Gaussian(id.xy, int2(0, 1));
}

[numthreads(8, 8, 1)]
void ComposePost(uint3 id : SV_DispatchThreadID)
{
    if (!InBounds(id.xy)) return;
    const float2 uv = (float2(id.xy) + 0.5) / float2(Width, Height);
    const float4 raw = Input.Load(int3(id.xy, 0));
    const float4 blur = Blurred.Load(int3(id.xy, 0));
    float3 color = raw.rgb;
    if (Style == 1)
    {
        const float center = 0.25 + 0.25 * (1.0 - cos(Time * 0.75));
        const float mask = 1.0 - smoothstep(0.24, 0.26, abs(uv.x - center));
        color = lerp(raw.rgb, blur.rgb, mask);
        const float rail = exp(-abs(uv.x - center) * Width * 0.7);
        color += rail * float3(0.10, 0.30, 0.33);
        const float handle = step(uv.y, 0.055) + step(0.945, uv.y);
        color += handle * exp(-abs(uv.x - center) * Width * 0.10) * float3(0.25, 0.80, 0.75);
    }
    else if (Style == 0)
    {
        const float pulse = 0.75 + 0.25 * sin(Time * 3.2);
        color = raw.rgb * 0.90 + blur.rgb * (0.80 + 0.25 * pulse);
        color *= 0.965 + 0.035 * sin(uv.y * Height * 3.14159265 + Time * 7.0);
    }
    Output[id.xy] = float4(saturate(color), raw.a);
}
