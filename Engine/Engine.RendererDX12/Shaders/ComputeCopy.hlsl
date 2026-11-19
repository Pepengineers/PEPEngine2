Texture2D<TEXEL> Source : register(t0);
RWTexture2D<TEXEL> Destination : register(u0);

[numthreads(16, 8, 1)]
void CopyTexture(uint3 thread : SV_DispatchThreadID)
{
    uint width, height;
    Destination.GetDimensions(width, height);
    if (thread.x < width && thread.y < height)
    {
        Destination[thread.xy] = Source.Load(int3(thread.xy, 0));
    }
}
