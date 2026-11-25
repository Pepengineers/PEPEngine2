cbuffer CloudFrame : register(b0)
{
    float4x4 InvViewProj;
    float3 CameraCm;
    uint ViewSteps;
    float3 SunDirection;
    uint LightSteps;
    float3 SunIlluminance;
    uint Seed;
    float2 WindMeters;
    float2 Padding;
};
RWTexture2D<float4> CloudLayer : register(u0);

static const float Base = 650.0;
static const float Top = 1150.0;
static const float Range = 9000.0;
static const float Extinction = 0.008;

float Hash(int3 cell)
{
    uint3 p = asuint(cell);
    uint h = p.x * 1597334677u ^ p.y * 3812015801u ^ p.z * 2798796415u ^ Seed;
    h = (h ^ (h >> 16)) * 0x7feb352du;
    h = (h ^ (h >> 15)) * 0x846ca68bu;
    return float((h ^ (h >> 16)) >> 8) * (1.0 / 16777216.0);
}

float Noise(float3 p)
{
    int3 i = int3(floor(p));
    float3 f = frac(p);
    f = f * f * (3.0 - 2.0 * f);
    float z0 = lerp(lerp(Hash(i), Hash(i + int3(1,0,0)), f.x),
                    lerp(Hash(i + int3(0,1,0)), Hash(i + int3(1,1,0)), f.x), f.y);
    float z1 = lerp(lerp(Hash(i + int3(0,0,1)), Hash(i + int3(1,0,1)), f.x),
                    lerp(Hash(i + int3(0,1,1)), Hash(i + int3(1,1,1)), f.x), f.y);
    return lerp(z0, z1, f.z);
}

float Density(float3 p, bool detail)
{
    float h = (p.y - Base) / (Top - Base);
    float envelope = smoothstep(0.0, 0.19, h) * (1.0 - smoothstep(0.52, 1.0, h));
    p.xz -= WindMeters;
    float weather = Noise(float3(p.xz / 1550.0, 17.0));
    float3 body = p / float3(445.0, 235.0, 445.0);
    float mass = 0.65 * weather + 0.35 * Noise(body);
    mass -= 0.39 + 0.19 * abs(h - 0.38);
    if (mass <= 0.0 || envelope <= 0.0) return 0.0;
    if (detail)
        mass -= 0.11 * (0.68 * Noise(body * 2.7) + 0.32 * Noise(body * 6.5));
    return saturate(mass * 4.2) * envelope;
}

float SunVisibility(float3 p)
{
    float span = min(1250.0, (Top - p.y) / max(SunDirection.y, 0.01));
    float stride = span / LightSteps;
    float opticalDepth = 0.0;
    for (uint i = 0; i < LightSteps; ++i)
        opticalDepth += Density(p + SunDirection * ((i + 0.5) * stride), false) * stride;
    return exp(-opticalDepth * Extinction * 1.4);
}

[numthreads(8, 8, 1)]
void TraceCloudsCS(uint3 id : SV_DispatchThreadID)
{
    uint width, height;
    CloudLayer.GetDimensions(width, height);
    if (any(id.xy >= uint2(width, height))) return;
    float2 ndc = (float2(id.xy) + 0.5) / float2(width, height) * float2(2,-2) + float2(-1,1);
    float4 farPoint = mul(float4(ndc, 0.000001, 1), InvViewProj);
    float3 ray = normalize(farPoint.xyz - CameraCm * farPoint.w);
    float3 origin = CameraCm * 0.01;
    float y = abs(ray.y) > 0.0001 ? ray.y : (ray.y < 0 ? -0.0001 : 0.0001);
    float bottom = (Base - origin.y) / y, top = (Top - origin.y) / y;
    float nearDistance = max(0.0, min(bottom, top));
    float farDistance = min(Range, max(bottom, top));
    if (farDistance <= nearDistance)
    {
        CloudLayer[id.xy] = 0;
        return;
    }

    float3 sun = SunIlluminance * smoothstep(0.0, 0.15, SunDirection.y);
    float dusk = 1.0 - smoothstep(0.02, 0.42, SunDirection.y);
    sun *= lerp(float3(1.0, 0.97, 0.92), float3(1.0, 0.55, 0.30), dusk);
    float3 ambient = lerp(float3(0.18, 0.23, 0.31), float3(0.046, 0.060, 0.094), dusk);
    float cosTheta = dot(ray, SunDirection);
    float phase = 0.62 + 0.72 * pow(saturate(0.5 + 0.5 * cosTheta), 6.0);
    float stride = (farDistance - nearDistance) / ViewSteps;
    float transmittance = 1.0;
    float3 radiance = 0;
    for (uint i = 0; i < ViewSteps && transmittance > 0.015; ++i)
    {
        float distance = nearDistance + (i + 0.5) * stride;
        float3 samplePoint = origin + ray * distance;
        float density = Density(samplePoint, true) * (1.0 - smoothstep(6200.0, Range, distance));
        if (density <= 0.0) continue;
        float opacity = 1.0 - exp(-density * stride * Extinction);
        float3 scatter = ambient + sun * (0.10 + phase * SunVisibility(samplePoint));
        radiance += transmittance * opacity * scatter;
        transmittance *= 1.0 - opacity;
    }
    CloudLayer[id.xy] = float4(radiance, 1.0 - transmittance);
}
