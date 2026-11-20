cbuffer Bar : register(b0)
{
    float2 Size;
    float Health;
    float Phase;
    uint Sparks;
};

struct Vertex
{
    float4 Position : SV_Position;
    float2 UV : TEXCOORD0;
    nointerpolation uint Instance : TEXCOORD1;
};

Vertex VSMain(uint vertex : SV_VertexID, uint instance : SV_InstanceID)
{
    // Instance zero paints the opaque bar before the sparks are blended over it.
    float2 uv = float2(vertex & 1, vertex >> 1) * 2;
    if (instance != 0)
    {
        uint random = instance * 2654435761u;
        random = (random ^ (random >> 16)) * 0x7feb352du;
        random ^= random >> 15;
        const float2 corners[3] = {float2(-2, 0), float2(2, -1.4), float2(1, 1.4)};
        uv = float2(frac(float(random & 65535) / 65536 + Phase) * Health,
                    float(random >> 16) / 65536) + corners[vertex] / Size;
    }
    Vertex output;
    output.Position = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    output.UV = uv;
    output.Instance = instance;
    return output;
}

float4 PSMain(Vertex input) : SV_Target
{
    if (input.Instance != 0)
    {
        clip(Health - input.UV.x);
        return float4(255, 195, 126, clamp(3200u / Sparks, 8u, 210u)) / 255;
    }
    if (input.UV.x > Health)
        return float4(float3(37, 18, 30) / 255, 1);

    const float2 uv = float2(input.UV.x / Health, input.UV.y);
    const float3 top = lerp(float3(112, 12, 40), float3(218, 57, 64), uv.x);
    const float3 bottom = lerp(float3(63, 12, 39), float3(143, 18, 46), uv.x);
    return float4(lerp(top, bottom, uv.y) / 255, 1);
}
