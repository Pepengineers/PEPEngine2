#include "CBufferStructures.hlsl"

ConstantBuffer<MainCB> CBMain : register(b0);

struct VertexIn
{
	float3 PosL    : POSITION;
};

struct VertexOut
{
	float4 PosH    : SV_POSITION;
};


VertexOut VS_FSQuad(uint vertexID : SV_VertexID)
{
    //full-screen quad
    
    float2 verts[3] =
    {
        float2(-1, -1),
        float2(-1, 3),
        float2(3, -1)
    };
    
    
    VertexOut vout;
    vout.PosH = float4(verts[vertexID], 0, 1);
    return vout;
}

float variation(float2 v1, float2 v2, float strength, float speed, float time)
{
    return sin(dot(normalize(v1), normalize(v2)) * strength + time * speed) / 100.0;
}

float3 paintCircle(float2 uv, float2 center, float rad, float width, float index, float time)
{
    float2 diff = center - uv;
    float len = length(diff);
    float scale = rad;
    float mult = fmod(index, 2.0) == 0.0 ? 1.0 : -1.0;
    len += variation(diff, float2(rad * mult, 1.0), 7.0 * scale, 2.0, time);
    len -= variation(diff, float2(1.0, rad * mult), 7.0 * scale, 2.0, time);
    float circle = smoothstep((rad - width) * scale, rad * scale, len) -
                   smoothstep(rad * scale, (rad + width) * scale, len);
    return float3(circle, circle, circle);
}

float3 paintRing(float2 uv, float2 center, float radius, float index, float time)
{
    float3 color = paintCircle(uv, center, radius, 0.075, index, time);
    color *= float3(0.3, 0.85, 1.0);
    color += paintCircle(uv, center, radius, 0.015, index, time);
    return color;
}

// https://www.shadertoy.com/view/MtGXWh
float4 PS(VertexOut pin) : SV_Target
{
    uint2 TexelCoord = pin.PosH.xy;
    float2 UV = TexelCoord / CBMain.RenderTargetSize;
    
    const float numRings = 20.0;
    const float2 center = float2(0.5, 0.5);
    const float spacing = 1.0 / numRings;
    const float slow = 30.0;
    const float cycleDur = 1.0;
    const float tunnelElongation = 0.25;
    
    float time = CBMain.TotalTime;
    float radius = fmod(time / slow, cycleDur);
    float3 color = float3(0.0, 0.0, 0.0);
    
    float border = 0.25;
    float2 bl = smoothstep(0.0, border, UV);
    float2 tr = smoothstep(0.0, border, 1.0 - UV);
    float edges = bl.x * bl.y * tr.x * tr.y;
    
    UV.x *= 1.5;
    UV.x -= 0.25;
    
    for (float i = 0.0; i < numRings; i++)
    {
        color += paintRing(UV, center, tunnelElongation * log(fmod(radius + i * spacing, cycleDur)), i, time);
        color += paintRing(UV, center, log(fmod(radius + i * spacing, cycleDur)), i, time);
    }
    
    color = lerp(color, float3(0.0, 0.0, 0.0), 1.0 - edges);
    color = lerp(color, float3(0.0, 0.0, 0.0), distance(UV, center));
    
    return float4(color, 1.0);
}