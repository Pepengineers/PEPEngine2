cbuffer RegionConstants : register(b0)
{
    uint2 AtlasOrigin;
    uint2 Extent;
    uint AtlasSlice;
    uint PayloadLayer;
};

Texture2DArray<float> DepthInput : register(t0);
RWTexture2DArray<float> PackedDepth : register(u0);

[numthreads(8, 8, 1)]
void PackCS(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= Extent)) return;
    PackedDepth[uint3(id.xy, PayloadLayer)] = DepthInput.Load(int4(id.xy + AtlasOrigin, AtlasSlice, 0));
}

float4 RestoreVS(uint vertex : SV_VertexID) : SV_Position
{
    float2 corner = float2((vertex << 1) & 2, vertex & 2);
    return float4(corner * float2(2, -2) + float2(-1, 1), 0, 1);
}

float RestorePS(float4 position : SV_Position) : SV_Depth
{
    uint2 pixel = uint2(position.xy) - AtlasOrigin;
    return DepthInput.Load(int4(pixel, PayloadLayer, 0));
}
