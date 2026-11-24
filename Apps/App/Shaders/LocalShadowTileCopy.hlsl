cbuffer Tile : register(b0)
{
    uint2 SourceOrigin;
    uint SourcePage;
    uint Side;
    uint2 TargetOrigin;
    uint TargetPage;
};

Texture2DArray<float> Source : register(t0);

#ifdef PACK_LOCAL_TILE
RWTexture2DArray<float> Target : register(u0);

[numthreads(8, 8, 1)]
void Pack(uint3 thread : SV_DispatchThreadID)
{
    if (all(thread.xy < Side))
        Target[uint3(TargetOrigin + thread.xy, TargetPage)] =
            Source.Load(int4(SourceOrigin + thread.xy, SourcePage, 0));
}
#else
float4 VS(uint vertex : SV_VertexID) : SV_Position
{
    return float4(float2(vertex == 2 ? 3 : -1, vertex == 1 ? 3 : -1), 0, 1);
}

float PS(float4 position : SV_Position) : SV_Depth
{
    const uint2 texel = SourceOrigin + uint2(position.xy) - TargetOrigin;
    return Source.Load(int4(texel, SourcePage, 0));
}
#endif
