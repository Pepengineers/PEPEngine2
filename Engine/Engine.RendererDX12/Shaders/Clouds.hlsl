#include "CloudNoise.hlsli"
cbuffer CloudParameters : register(b0)
{
    uint OutputSize; uint Octaves; uint Implementation; uint Enabled;
    float Scale; float Coverage; float Softness; float Padding;
    float3 HorizonColor; float PhaseX;
    float3 ZenithColor; float PhaseY;
    float3 CloudColor; float Padding2;
    float4 Padding3;
};
RWTexture2DArray<float4> Output : register(u0);

[numthreads(8, 8, 1)]
void CloudsCS(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= OutputSize) || id.z >= 6) return;
    float2 uv = (float2(id.xy) + 0.5f) / OutputSize * 2.0f - 1.0f;
    float3 direction;
    if (id.z == 0) direction = float3(1, -uv.y, -uv.x);
    else if (id.z == 1) direction = float3(-1, -uv.y, uv.x);
    else if (id.z == 2) direction = float3(uv.x, 1, uv.y);
    else if (id.z == 3) direction = float3(uv.x, -1, -uv.y);
    else if (id.z == 4) direction = float3(uv.x, -uv.y, 1);
    else direction = float3(-uv.x, -uv.y, -1);
    direction = normalize(direction);
    float3 sky = lerp(HorizonColor, ZenithColor, abs(direction.y));
    if (Enabled == 0)
    {
        Output[id] = float4(sky, 0);
        return;
    }
    float density = CloudDirectionDensity(direction, Scale, float2(PhaseX, PhaseY), Octaves, Implementation);
    float opacity = smoothstep(1.0f - Coverage - Softness, 1.0f - Coverage + Softness, density);
    if (Coverage == 0) opacity = 0;
    if (Coverage == 1) opacity = 1;
    Output[id] = float4(lerp(sky, CloudColor, opacity), 1);
}
