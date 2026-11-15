cbuffer Parameters : register(b0)
{
    uint Width, Height, SourceWidth, SourceHeight, SourceOffset, DestinationOffset;
};
Texture2D<float> Depth : register(t0);
RWStructuredBuffer<float> Pyramid : register(u0);

[numthreads(8, 8, 1)]
void CS(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Width || id.y >= Height) return;
    float depth;
    if (SourceWidth == 0) depth = Depth[id.xy];
    else
    {
        uint2 a = min(id.xy * 2, uint2(SourceWidth, SourceHeight) - 1);
        uint2 b = min(a + 1, uint2(SourceWidth, SourceHeight) - 1);
        depth = min(min(Pyramid[SourceOffset + a.y * SourceWidth + a.x], Pyramid[SourceOffset + a.y * SourceWidth + b.x]),
            min(Pyramid[SourceOffset + b.y * SourceWidth + a.x], Pyramid[SourceOffset + b.y * SourceWidth + b.x]));
    }
    Pyramid[DestinationOffset + id.y * Width + id.x] = depth;
}
