#include "PBRMath.hlsli"
Texture2D<float4> Color : register(t0);
RWTexture2D<float4> Result : register(u0);
[numthreads(8, 8, 1)]
void CS(uint2 pixel : SV_DispatchThreadID)
{
    uint width, height; Result.GetDimensions(width, height);
    if (any(pixel >= uint2(width, height))) return;
    float4 color = Color[pixel];
    Result[pixel] = float4(ToneMapToSRGB(color.r), ToneMapToSRGB(color.g), ToneMapToSRGB(color.b), saturate(color.a));
}
