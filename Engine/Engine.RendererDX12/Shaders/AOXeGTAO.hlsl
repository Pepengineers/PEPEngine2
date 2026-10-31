#define XE_GTAO_FP32_DEPTHS 1
#define XE_GTAO_USE_HALF_FLOAT_PRECISION 0
#define XE_GTAO_USE_DEFAULT_CONSTANTS 1
#define VA_SATURATE(x) saturate(x)
#include "XeGTAO/XeGTAO.h"
#include "XeGTAO/XeGTAO.hlsli"
cbuffer GTAOBuffer : register(b0) { GTAOConstants GTAO; }
cbuffer ResolveParameters : register(b1)
{
    float4x4 Projection;
    float4x4 InverseProjection;
    uint2 Size;
    uint2 WorkingSize;
    float Radius;
    float Intensity;
    float Bias;
    uint Samples;
    uint Horizontal;
    uint RestorePackedScale;
    uint2 Padding;
};
SamplerState PointClamp : register(s0);

#if AO_STAGE == 0
Texture2D<float> RawDepth : register(t0);
RWTexture2D<float> Depth0 : register(u0);
RWTexture2D<float> Depth1 : register(u1);
RWTexture2D<float> Depth2 : register(u2);
RWTexture2D<float> Depth3 : register(u3);
RWTexture2D<float> Depth4 : register(u4);
[numthreads(8, 8, 1)]
void PrefilterCS(uint2 dispatch : SV_DispatchThreadID, uint2 lane : SV_GroupThreadID)
{
    XeGTAO_PrefilterDepths16x16(dispatch, lane, GTAO, RawDepth, PointClamp, Depth0, Depth1, Depth2, Depth3, Depth4);
}
#elif AO_STAGE == 1
Texture2D<float> WorkingDepth : register(t0);
Texture2D<float4> ViewNormals : register(t1);
RWTexture2D<uint> PackedAO : register(u0);
RWTexture2D<unorm float> Edges : register(u1);
[numthreads(8, 8, 1)]
void MainCS(uint2 pixel : SV_DispatchThreadID)
{
    uint index = HilbertIndex(pixel.x, pixel.y);
    float2 noise = frac(0.5f + index * float2(0.754877666f, 0.569840296f));
    float steps = Samples <= 8 ? 2 : 3;
    float slices = max(1, floor(Samples / (steps * 2)));
    XeGTAO_MainPass(pixel, slices, steps, noise, ViewNormals[pixel].xyz, GTAO, WorkingDepth, PointClamp, PackedAO, Edges);
}
#elif AO_STAGE == 2
Texture2D<uint> PackedAO : register(t0);
Texture2D<float> Edges : register(t1);
RWTexture2D<uint> FilteredAO : register(u0);
[numthreads(8, 8, 1)]
void DenoiseCS(uint2 pixel : SV_DispatchThreadID)
{
    XeGTAO_Denoise(pixel * uint2(2, 1), GTAO, PackedAO, Edges, PointClamp, FilteredAO, false);
}
[numthreads(8, 8, 1)]
void FinalDenoiseCS(uint2 pixel : SV_DispatchThreadID)
{
    XeGTAO_Denoise(pixel * uint2(2, 1), GTAO, PackedAO, Edges, PointClamp, FilteredAO, true);
}
#else
Texture2D<uint> PackedAO : register(t0);
Texture2D<float> RawDepth : register(t1);
RWTexture2D<float> Visibility : register(u0);
[numthreads(8, 8, 1)]
void ResolveCS(uint2 pixel : SV_DispatchThreadID)
{
    if (any(pixel >= Size)) return;
    float value = PackedAO[pixel] / 255.0f;
    if (RestorePackedScale != 0) value *= XE_GTAO_OCCLUSION_TERM_SCALE;
    Visibility[pixel] = RawDepth[pixel] <= 1e-8f ? 1 : saturate(1 - (1 - saturate(value)) * Intensity);
}
#endif
