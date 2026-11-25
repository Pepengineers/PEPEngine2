/*
 * Copyright (c) 2014-2021, NVIDIA CORPORATION.  All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * SPDX-FileCopyrightText: Copyright (c) 2014-2021 NVIDIA CORPORATION
 * SPDX-License-Identifier: Apache-2.0
 */
// Adapted from nvpro-samples/gl_ssao hbao.frag.glsl (AO_DEINTERLEAVED=0)
// and hbao_blur.frag.glsl to HLSL compute, reverse-Z and world-space normals.
// Jitter is generated from a 4x4 pixel tile instead of a random texture.
// https://github.com/nvpro-samples/gl_ssao
#include "CBufferStructures.hlsli"

cbuffer Parameters : register(b0)
{
    float Radius, Intensity, Bias, Sharpness;
    uint Directions, Steps, BlurAxis;
    float FadeStart, FadeEnd;
};
ConstantBuffer<CameraCB> Camera : register(b1);
Texture2D<float> Depth : register(t0);
Texture2D<float4> Normals : register(t1);
Texture2D<float> InputAO : register(t2);
RWTexture2D<float> OutputAO : register(u0);

float LinearDepth(float depth)
{ return Camera.NearPlane * Camera.FarPlane / (Camera.NearPlane + depth * (Camera.FarPlane - Camera.NearPlane)); }

float3 WorldPosition(uint2 pixel, uint2 size, float depth)
{
    float2 ndc = (float2(pixel) + 0.5f) / size * float2(2, -2) + float2(-1, 1);
    float4 world = mul(float4(ndc, depth, 1), Camera.InvViewProj);
    return world.xyz / world.w;
}

float2 PixelJitter(uint2 pixel)
{
    uint seed = (pixel.x & 3u) + (pixel.y & 3u) * 4u + 1u;
    seed = seed * 747796405u + 2891336453u;
    uint bits = ((seed >> ((seed >> 28u) + 4u)) ^ seed) * 277803737u;
    bits = (bits >> 22u) ^ bits;
    return float2(bits & 0xffffu, bits >> 16u) / 65536.0f;
}

[numthreads(8, 8, 1)]
void HBAOCS(uint3 id : SV_DispatchThreadID)
{
    uint2 size, pixel = id.xy;
    Depth.GetDimensions(size.x, size.y);
    if (any(pixel >= size)) return;
    float depth = Depth[pixel];
    if (depth <= 0 || Intensity == 0) { OutputAO[pixel] = 1; return; }
    float3 center = WorldPosition(pixel, size, depth);
    float3 normal = SafeNormalize(Normals[pixel].xyz);
    float3 cameraUp = float3(Camera.View._12, Camera.View._22, Camera.View._32);
    float4 clip = mul(float4(center, 1), Camera.ViewProj);
    float radiusPixels = abs(mul(float4(cameraUp * Radius, 0), Camera.ViewProj).y) * (0.5f * size.y) / clip.w;
    float stepSizePixels = radiusPixels / (Steps + 1);
    float2 jitter = PixelJitter(pixel);
    float angleStep = 6.28318530718f / Directions;
    float occlusion = 0;
    for (uint direction = 0; direction < Directions; ++direction)
    {
        float angle = (direction + jitter.x) * angleStep;
        float2 ray = float2(cos(angle), sin(angle));
        float rayPixels = jitter.y * stepSizePixels + 1;
        for (uint step = 0; step < Steps; ++step)
        {
            int2 q = int2(pixel) + int2(round(rayPixels * ray));
            rayPixels += stepSizePixels;
            if (any(q < 0) || any(q >= int2(size))) continue;
            float otherDepth = Depth[q];
            if (otherDepth <= 0) continue;
            float3 delta = WorldPosition(q, size, otherDepth) - center;
            float distanceSquared = dot(delta, delta);
            if (distanceSquared < 1e-6f) continue;
            float nDotDelta = dot(normal, delta) * rsqrt(distanceSquared);
            occlusion += saturate(nDotDelta - Bias) * saturate(1 - distanceSquared / (Radius * Radius));
        }
    }
    float visibility = saturate(1 - 2 * occlusion / (Directions * Steps * (1 - Bias)));
    OutputAO[pixel] = pow(visibility, Intensity);
}

// NVIDIA's separable cross-bilateral blur: radius 3, sigma 1.5, depth rejection.
[numthreads(8, 8, 1)]
void BlurCS(uint3 id : SV_DispatchThreadID)
{
    uint2 size, pixel = id.xy;
    Depth.GetDimensions(size.x, size.y);
    if (any(pixel >= size)) return;
    if (Depth[pixel] <= 0 || Intensity == 0) { OutputAO[pixel] = 1; return; }
    float centerDepth = LinearDepth(Depth[pixel]);
    float sum = InputAO[pixel], weightSum = 1;
    for (int offset = -3; offset <= 3; ++offset)
    {
        if (offset == 0) continue;
        int2 q = int2(pixel) + (BlurAxis == 0 ? int2(offset, 0) : int2(0, offset));
        if (any(q < 0) || any(q >= int2(size)) || Depth[q] <= 0) continue;
        float depthDelta = (LinearDepth(Depth[q]) - centerDepth) * Sharpness;
        float weight = exp2(-offset * offset / 4.5f - depthDelta * depthDelta);
        sum += InputAO[q] * weight;
        weightSum += weight;
    }
    float visibility = sum / weightSum;
    if (BlurAxis != 0)
    {
        float fade = saturate((FadeEnd - centerDepth) / (FadeEnd - FadeStart));
        visibility = lerp(1, visibility, fade);
    }
    OutputAO[pixel] = visibility;
}
