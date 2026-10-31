RWTexture2D<float4> Output : register(u0);
[numthreads(8, 8, 1)]
void ClearReflectionsCS(uint2 pixel : SV_DispatchThreadID)
{
    uint width, height;
    Output.GetDimensions(width, height);
    if (all(pixel < uint2(width, height))) Output[pixel] = 0;
}
