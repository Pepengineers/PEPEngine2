// Copyright (C) 2016-2021, Intel Corporation
// SPDX-License-Identifier: MIT
#include "CBufferStructures.hlsli"
#define XE_GTAO_USE_HALF_FLOAT_PRECISION 0
#define XE_GTAO_FP32_DEPTHS
#define XE_GTAO_USE_DEFAULT_CONSTANTS 0
#define VA_SATURATE saturate
#include "../../../External/External.Libraries/XeGTAO/XeGTAO.hlsli"

#ifndef XE_GTAO_SLICES
#define XE_GTAO_SLICES 3
#endif
#ifndef XE_GTAO_STEPS
#define XE_GTAO_STEPS 3
#endif

cbuffer DispatchParameters : register(b0)
{
    float Intensity, FadeStart, FadeEnd;
    uint FinalDenoise;
    float InputScale;
};
ConstantBuffer<GTAOConstants> Xe : register(b1);
ConstantBuffer<CameraCB> Camera : register(b2);
Texture2D<float> Depth : register(t0);
Texture2D<float4> Normals : register(t1);
Texture2D<float> WorkingDepth : register(t2);
Texture2D<uint> InputAO : register(t3);
Texture2D<float> Edges : register(t4);
RWTexture2D<float> OutputAO : register(u0);
RWTexture2D<float> Depth0 : register(u1);
RWTexture2D<float> Depth1 : register(u2);
RWTexture2D<float> Depth2 : register(u3);
RWTexture2D<float> Depth3 : register(u4);
RWTexture2D<float> Depth4 : register(u5);
RWTexture2D<uint> WorkingAO : register(u6);
RWTexture2D<unorm float> OutputEdges : register(u7);
SamplerState PointClamp : register(s1);

[numthreads(8, 8, 1)]
void PrefilterCS(uint3 id : SV_DispatchThreadID, uint3 thread : SV_GroupThreadID)
{
    // Every thread participates in the upstream group barriers, including padding.
    XeGTAO_PrefilterDepths16x16(id.xy, thread.xy, Xe, Depth, PointClamp, Depth0, Depth1, Depth2, Depth3, Depth4);
}

[numthreads(8, 8, 1)]
void MainCS(uint3 id : SV_DispatchThreadID)
{
    uint2 size;
    WorkingDepth.GetDimensions(size.x, size.y);
    uint2 pixel = min(id.xy, uint2(Xe.ViewportSize) - 1);
    if (Depth[pixel] <= 0 || Intensity == 0)
    {
        WorkingAO[id.xy] = uint(255.0f / XE_GTAO_OCCLUSION_TERM_SCALE + 0.5f);
        OutputEdges[id.xy] = 0;
        return;
    }
    float3 normal = SafeNormalize(mul(Normals[pixel].xyz, (float3x3)Camera.View));
    normal.z = -normal.z; // XeGTAO uses positive-forward view positions.
    GTAOConstants constants = Xe;
    constants.NDCToViewMul *= float2(size) / float2(Xe.ViewportSize);
    constants.ViewportSize = int2(size);
    constants.ViewportPixelSize = 1.0f / size;
    uint index = HilbertIndex(id.x, id.y) + 288 * (Xe.NoiseIndex % 64);
    float2 noise = frac(0.5f + index * float2(0.7548776662f, 0.5698402910f));
    XeGTAO_MainPass(id.xy, XE_GTAO_SLICES, XE_GTAO_STEPS, noise, normal,
        constants, WorkingDepth, PointClamp, WorkingAO, OutputEdges);
}

[numthreads(8, 8, 1)]
void DenoiseCS(uint3 id : SV_DispatchThreadID)
{
    uint2 size;
    InputAO.GetDimensions(size.x, size.y);
    GTAOConstants constants = Xe;
    constants.ViewportSize = int2(size);
    constants.ViewportPixelSize = 1.0f / size;
    XeGTAO_Denoise(uint2(id.x * 2, id.y), constants, InputAO, Edges, PointClamp, WorkingAO, FinalDenoise != 0);
}

[numthreads(8, 8, 1)]
void ResolveCS(uint3 id : SV_DispatchThreadID)
{
    uint2 size;
    OutputAO.GetDimensions(size.x, size.y);
    if (any(id.xy >= size)) return;
    float raw = Depth[id.xy];
    if (raw <= 0 || Intensity == 0) { OutputAO[id.xy] = 1; return; }
    float depth = XeGTAO_ScreenSpaceToViewSpaceDepth(raw, Xe);
    float fade = saturate((FadeEnd - depth) / (FadeEnd - FadeStart));
    float visibility = saturate(InputAO[id.xy] * (InputScale / 255.0f));
    OutputAO[id.xy] = saturate(1 - (1 - visibility) * Intensity * fade);
}
