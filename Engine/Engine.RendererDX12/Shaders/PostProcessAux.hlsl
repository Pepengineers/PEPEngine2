cbuffer Parameters : register(b0)
{
    uint2 OutputSize, InputSize;
    float2 Jitter;
};
Texture2D<float> InputDepth : register(t0);
Texture2D<float2> InputMotion : register(t1);
RWTexture2D<float> OutputDepth : register(u0);
RWTexture2D<float2> OutputMotion : register(u1);

[numthreads(8, 8, 1)]
void CS(uint2 pixel : SV_DispatchThreadID)
{
    if (any(pixel >= OutputSize)) return;
    // shortcut: nearest source geometry preserves depth/velocity edges; use output-resolution geometry for subpixel reconstruction.
    float2 position = (float2(pixel) + 0.5) / OutputSize * InputSize - Jitter;
    int2 source = clamp(int2(floor(position)), int2(0, 0), int2(InputSize) - 1);
    OutputDepth[pixel] = InputDepth[source];
    OutputMotion[pixel] = InputMotion[source];
}
