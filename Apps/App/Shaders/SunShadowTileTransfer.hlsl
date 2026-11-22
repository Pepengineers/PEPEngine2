cbuffer Tile : register(b0)
{
    uint2 Origin;
    uint Slice;
    uint Resolution;
};

#ifdef PACK_SUN_TILE
Texture2D<float> Atlas : register(t0);
RWTexture2DArray<float> Tiles : register(u0);

[numthreads(8, 8, 1)]
void Pack(uint3 thread : SV_DispatchThreadID)
{
    if (all(thread.xy < Resolution))
        Tiles[uint3(thread.xy, Slice)] = Atlas.Load(int3(Origin + thread.xy, 0));
}
#else
Texture2DArray<float> Tiles : register(t0);

float4 VS(uint vertex : SV_VertexID) : SV_Position
{
    return float4(float2(vertex == 2 ? 3 : -1, vertex == 1 ? 3 : -1), 0, 1);
}

float PS(float4 position : SV_Position) : SV_Depth
{
    return Tiles.Load(int4(uint2(position.xy) - Origin, Slice, 0));
}
#endif
