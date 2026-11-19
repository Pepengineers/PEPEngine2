Texture2D<float4> Source : register(t0);
RWTexture2D<float4> Preview : register(u0);

cbuffer Settings : register(b0)
{
    uint2 TileOrigin;
    uint TileSize;
    uint View;
    float NearPlane;
    float FarPlane;
    float DepthRange;
};

[numthreads(8, 8, 1)]
void CS(uint3 id : SV_DispatchThreadID)
{
    uint width, height;
    Source.GetDimensions(width, height);
    uint2 pixel = min(uint2((id.xy + 0.5f) * float2(width, height) / TileSize), uint2(width - 1, height - 1));
    float4 value = Source.Load(int3(pixel, 0));
    float3 color = value.rgb;
    if (View == 1) color = value.rrr;
    if (View == 2)
    {
        float distanceCm = NearPlane * FarPlane / (NearPlane + value.r * (FarPlane - NearPlane));
        color = (value.r > 0 ? 1 - saturate(distanceCm / DepthRange) : 0).xxx;
    }
    if (View == 3) color = value.rgb * 0.5f + 0.5f;
    if (View == 4) color = float3(value.rg * 16 + 0.5f, 0.5f);
    if (View == 5) color = max(color, 0) / (1 + max(color, 0));
    if (View >= 6) color = value[View - 6].xxx;
    Preview[TileOrigin + id.xy] = float4(all(isfinite(color)) ? saturate(color) : float3(1, 0, 1), 1);
}
