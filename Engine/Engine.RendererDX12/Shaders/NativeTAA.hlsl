// Adapted from Microsoft Xbox-GDK-Samples TemporalAntialiasing/TemporalResolve.hlsl
// (73f8aa373138ecd6331a05ac908a88c287602db8), and MiniEngine TemporalBlendCS.hlsl / ShaderUtility.hlsli.
// Copyright (c) Microsoft Corporation.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
#include "CBufferStructures.hlsl"
cbuffer Parameters : register(b0)
{
    float4x4 PreviousViewProjection;
    uint2 Size;
    uint Valid;
    float HistoryWeight;
    float VarianceGamma;
    float2 DepthJitterDeltaUV;
    float Padding;
    uint2 InputSize;
    float2 InputJitterUV;
};
ConstantBuffer<CameraCB> Camera : register(b1);
Texture2D<float4> Color : register(t0);
Texture2D<float> Depth : register(t1);
Texture2D<float2> Motion : register(t2);
Texture2D<float4> History : register(t3);
Texture2D<float> HistoryDepth : register(t4); // positive previous view distance; zero is sky
RWTexture2D<float4> Result : register(u0);
RWTexture2D<float> ResultDepth : register(u1);
SamplerState LinearClamp : register(s0);

float3 SafeColor(float3 c) { return all(isfinite(c)) ? clamp(c, 0, 65504.0f) : 0; }
float Luminance(float3 c) { return dot(c, float3(0.212671f, 0.715160f, 0.072169f)); }
float3 TM(float3 c) { return c / (1 + Luminance(c)); }
float3 ITM(float3 c) { return c / max(1e-6f, 1 - Luminance(c)); }

void ResolvePixel(uint2 p, bool upscale)
{
    if (any(p >= Size)) return;
    float2 uv = (float2(p) + 0.5f) / Size;
    uint2 inputSize = upscale ? InputSize : Size;
    float2 inputUV = uv;
    uint2 inputPixel = p;
    if (upscale)
    {
        // Project the stable output sample into the current jittered render grid.
        inputUV += InputJitterUV;
        inputPixel = min(uint2(saturate(inputUV) * inputSize), inputSize - 1);
    }
    float4 current = upscale ? Color.SampleLevel(LinearClamp, inputUV, 0) : Color[p];
    current.rgb = SafeColor(current.rgb);
    current.a = isfinite(current.a) ? saturate(current.a) : 1;
    float depth = Depth[inputPixel];
    // Depth and motion describe one surface. Unproject depth at its actual texel center.
    float2 depthUV = upscale ? (float2(inputPixel) + 0.5f) / inputSize : uv;
    float4 world = mul(float4(depthUV * float2(2, -2) + float2(-1, 1), depth, 1), Camera.InvViewProj);
    bool surface = isfinite(depth) && depth > 0 && all(isfinite(world)) && abs(world.w) > 1e-8f;
    float distance = 0;
    float4 previous = 0;
    if (surface)
    {
        world /= world.w;
        distance = -mul(world, Camera.View).z;
        previous = mul(world, PreviousViewProjection);
    }
    ResultDepth[p] = surface && isfinite(distance) && distance > 0 ? distance : 0;
    float2 motion = Motion[inputPixel]; // prevNDC-currentNDC, unjittered
    float2 previousUV = uv + motion * float2(0.5f, -0.5f);
    float2 border = 0.5f / Size;
    bool valid = Valid != 0 && isfinite(depth) && (surface || depth == 0) && all(isfinite(motion)) &&
        all(previousUV >= border) && all(previousUV <= 1 - border);
    float weight = 0;
    float3 history = current.rgb;
    [branch] if (valid)
    {
        // TAAU stores depth on the stable output grid. Native TAA retains the jitter delta correction.
        float2 historyDepthUV = upscale ? previousUV : previousUV + DepthJitterDeltaUV;
        float4 oldDepths = HistoryDepth.Gather(LinearClamp, historyDepthUV);
        valid = false;
        [unroll] for (uint i = 0; i < 4; ++i)
        {
            float oldDepth = oldDepths[i];
            valid = valid || (isfinite(oldDepth) && (surface ? all(isfinite(previous)) && previous.w > 0 &&
                abs(oldDepth - previous.w) <= max(0.01f, previous.w * 0.02f) && oldDepth > 0 : oldDepth == 0));
        }
        [branch] if (valid)
        {
            float3 oldColor = History.SampleLevel(LinearClamp, previousUV, 0).rgb;
            if (all(isfinite(oldColor)))
            {
                float3 mean = 0, moment = 0;
                // Normalized HDR prevents overflow in the second moment.
                float scale = max(1.0f, max(current.r, max(current.g, current.b)));
                [unroll] for (int y = -1; y <= 1; ++y)
                    [unroll] for (int x = -1; x <= 1; ++x)
                    {
                        int2 q = clamp(int2(inputPixel) + int2(x, y), 0, int2(inputSize) - 1);
                        float3 c = SafeColor(Color[q].rgb) / scale;
                        mean += c / 9;
                        moment += c * c / 9;
                    }
                float3 sigma = sqrt(max(0, moment - mean * mean));
                float3 extent = min(65504.0f, VarianceGamma) * sigma;
                // GDK variance clipping; reject changes outside the neighborhood, not subpixel noise inside it.
                history = max(0, clamp(oldColor / scale, mean - extent, mean + extent) * scale);
                float difference = length(TM(oldColor) - TM(history));
                float speed = length(motion * float2(0.5f, -0.5f) * Size);
                weight = HistoryWeight * saturate(1 - difference) * saturate(1 - speed / 64.0f);
            }
        }
    }
    // MiniEngine blends HDR through its invertible luminance map to suppress bright-sample flicker.
    float3 resolved = weight > 0 ? ITM(lerp(TM(current.rgb), TM(history), weight)) : current.rgb;
    Result[p] = float4(SafeColor(resolved), current.a);
}

[numthreads(8, 8, 1)]
void Resolve(uint2 p : SV_DispatchThreadID)
{
    ResolvePixel(p, false);
}

[numthreads(8, 8, 1)]
void ResolveUpscaled(uint2 p : SV_DispatchThreadID)
{
    ResolvePixel(p, true);
}
