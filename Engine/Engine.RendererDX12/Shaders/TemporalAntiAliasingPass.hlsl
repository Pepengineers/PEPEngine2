#include "CBufferStructures.hlsli"
#include "FullScreenVS.hlsl"

cbuffer Parameters : register(b0)
{
    float2 Jitter;
    uint HistoryValid;
    float HistoryWeight;
};
ConstantBuffer<CameraCB> Camera : register(b1);
Texture2D<float4> Color : register(t0);
Texture2D<float> Depth : register(t1);
Texture2D<float2> Velocity : register(t2);
Texture2D<float> TransparencyMask : register(t3);
Texture2D<float4> History : register(t4);
SamplerState samPointClamp : register(s1);
SamplerState samLinearClamp : register(s3);

float3 DecodeSRGB(float3 color)
{
    float3 low = color / 12.92f;
    float3 high = pow((color + 0.055f) / 1.055f, 2.4f);
    return float3(color.r <= 0.04045f ? low.r : high.r,
        color.g <= 0.04045f ? low.g : high.g, color.b <= 0.04045f ? low.b : high.b);
}

float3 EncodeSRGB(float3 color)
{
    color = saturate(color);
    float3 low = color * 12.92f;
    float3 high = 1.055f * pow(color, 1.0f / 2.4f) - 0.055f;
    return float3(color.r <= 0.0031308f ? low.r : high.r,
        color.g <= 0.0031308f ? low.g : high.g, color.b <= 0.0031308f ? low.b : high.b);
}

float3 ToYCoCg(float3 rgb)
{ return float3(dot(rgb, float3(0.25f, 0.5f, 0.25f)), 0.5f * (rgb.r - rgb.b), dot(rgb, float3(-0.25f, 0.5f, -0.25f))); }

float3 FromYCoCg(float3 color)
{ return float3(color.x + color.y - color.z, color.x + color.z, color.x - color.y - color.z); }

float NormalizedLinearDepth(float depth)
{ return Camera.NearPlane / (Camera.NearPlane + depth * (Camera.FarPlane - Camera.NearPlane)); }

struct PS_OUTPUT
{
    float4 Color : SV_TARGET0;
    float4 History : SV_TARGET1;
};

PS_OUTPUT PS(VSOutput input)
{
    uint width, height;
    Color.GetDimensions(width, height);
    float2 size = float2(width, height);
    float2 texel = 1.0f / size;
    float2 uv = input.Position.xy * texel;
    // The projection offsets rasterized geometry by -Jitter pixels. History is unjittered.
    float2 currentUV = uv - Jitter * texel;
    float3 current = DecodeSRGB(Color.SampleLevel(samLinearClamp, currentUV, 0).rgb);
    int2 centerPixel = clamp(int2(currentUV * size), int2(0, 0), int2(width, height) - 1);
    int2 pixel = centerPixel;
    float depth = Depth.Load(int3(pixel, 0));
    // Color reconstruction covers both sides of a silhouette. Use a matching,
    // foreground-dilated depth and velocity so jitter does not alternate history
    // between the foreground and background. Reversed Z: greater depth is nearer.
    [unroll] for (int y = -1; y <= 1; ++y)
    {
        [unroll] for (int x = -1; x <= 1; ++x)
        {
            int2 neighbor = clamp(centerPixel + int2(x, y), int2(0, 0), int2(width, height) - 1);
            float neighborDepth = Depth.Load(int3(neighbor, 0));
            if (neighborDepth > depth)
            {
                depth = neighborDepth;
                pixel = neighbor;
            }
        }
    }
    float normalizedDepth = depth > 0 ? NormalizedLinearDepth(depth) : 0;
    float2 velocity = Velocity.Load(int3(pixel, 0));
    // Existing motion vectors are previous minus current in unjittered NDC.
    float2 previousUV = uv + velocity * float2(0.5f, -0.5f);
    float3 resolved = current;

    if (HistoryValid && all(isfinite(velocity)) &&
        all(previousUV >= 0.5f * texel) && all(previousUV <= 1.0f - 0.5f * texel))
    {
        float previousDepth = History.SampleLevel(samPointClamp, previousUV, 0).a;
        bool validDepth = depth == 0 && previousDepth == 0;
        if (depth > 0 && previousDepth > 0)
        {
            float2 ndc = (float2(pixel) + 0.5f) * texel * float2(2, -2) + float2(-1, 1);
            float4 world = mul(float4(ndc, depth, 1), Camera.InvViewProj);
            float4 previousClip = mul(world, Camera.PrevViewProjNoJitter);
            if (previousClip.w > 1e-5f)
            {
                float projectedDepth = previousClip.z / previousClip.w;
                float expectedDepth = NormalizedLinearDepth(saturate(projectedDepth));
                validDepth = projectedDepth > 0 && projectedDepth <= 1 &&
                    abs(previousDepth - expectedDepth) <= max(0.02f * expectedDepth, 1e-6f);
            }
        }
        if (validDepth)
        {
            float3 minimum = ToYCoCg(current), maximum = minimum;
            [unroll] for (int y = -1; y <= 1; ++y)
            {
                [unroll] for (int x = -1; x <= 1; ++x)
                {
                    float3 neighbor = ToYCoCg(DecodeSRGB(Color.SampleLevel(samLinearClamp, currentUV + float2(x, y) * texel, 0).rgb));
                    minimum = min(minimum, neighbor);
                    maximum = max(maximum, neighbor);
                }
            }
            float3 history = History.SampleLevel(samLinearClamp, previousUV, 0).rgb;
            history = FromYCoCg(clamp(ToYCoCg(history), minimum, maximum));
            float coverage = saturate(TransparencyMask.SampleLevel(samLinearClamp, currentUV, 0));
            float motionPixels = length(velocity * size * float2(0.5f, -0.5f));
            float weight = HistoryWeight * (1.0f - coverage) * lerp(1.0f, 0.7f, saturate(motionPixels / 8.0f));
            resolved = lerp(current, history, weight);
        }
    }

    PS_OUTPUT output;
    output.Color = float4(EncodeSRGB(resolved), 1);
    output.History = float4(resolved, normalizedDepth);
    return output;
}
