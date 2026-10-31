#if defined(SSSR_PREPARE_MOTION)
cbuffer MotionParameters : register(b0)
{
    float2 JitterDelta;
};
Texture2D<float2> Motion : register(t0);
RWTexture2D<float2> Output : register(u0);
[numthreads(8, 8, 1)]
void PrepareMotionCS(uint2 pixel : SV_DispatchThreadID)
{
    uint width, height;
    Output.GetDimensions(width, height);
    if (all(pixel < uint2(width, height))) Output[pixel] = Motion[pixel] + JitterDelta;
}
#else
RWTexture2D<float4> Output : register(u0);
[numthreads(8, 8, 1)]
void ClearReflectionsCS(uint2 pixel : SV_DispatchThreadID)
{
    uint width, height;
    Output.GetDimensions(width, height);
    if (all(pixel < uint2(width, height))) Output[pixel] = 0;
}
#endif
