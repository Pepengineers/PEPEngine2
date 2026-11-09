cbuffer CollisionView : register(b0)
{
    row_major float4x4 ViewProjection;
    float4 Tint;
};

float4 VS(float3 position : POSITION) : SV_Position
{
    return mul(float4(position, 1.0), ViewProjection);
}

float4 PS() : SV_Target
{
    return Tint;
}
