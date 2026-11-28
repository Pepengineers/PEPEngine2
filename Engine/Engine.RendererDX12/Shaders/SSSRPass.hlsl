#include "CBufferStructures.hlsli"
#include "PBRMath.hlsli"

cbuffer Parameters : register(b0)
{
    float2 JitterDeltaUV;
    float MaxRoughness, RoughnessFade, Intensity;
    uint HasAmbientOcclusion;
};
ConstantBuffer<CameraCB> Camera : register(b1);
Texture2D<float4> InputColor : register(t0);
Texture2D<float> Depth : register(t1);
Texture2D<float4> NormalRoughness : register(t2);
Texture2D<float4> F0Baked : register(t3);
Texture2D<float2> Velocity : register(t4);
TextureCube<float4> PrefilteredEnvironment : register(t5);
Texture2D<float4> EnvironmentBRDF : register(t6);
Texture2D<float4> Reflections : register(t7);
Texture2D<float4> Fallback : register(t8);
Texture2D<float> AmbientOcclusion : register(t9);
RWTexture2D<float> Roughness : register(u0);
RWTexture2D<float2> MotionVectors : register(u1);
RWTexture2D<float4> SceneColor : register(u2);
RWTexture2D<float4> FallbackOutput : register(u3);
SamplerState samLinearClamp : register(s3);

float3 ReconstructPosition(uint2 pixel, uint2 size)
{
    float2 ndc = (float2(pixel) + 0.5f) / size * float2(2, -2) + float2(-1, 1);
    float4 world = mul(float4(ndc, Depth[pixel], 1), Camera.InvViewProj);
    return world.xyz / world.w;
}

float ReflectionWeight(uint2 pixel)
{
    float4 surface = NormalRoughness[pixel], f0 = F0Baked[pixel];
    if (Intensity <= 0 || Depth[pixel] <= 0 || f0.a > 0.5f || surface.w >= MaxRoughness
        || dot(surface.xyz, surface.xyz) < 1e-8f || !any(f0.rgb > 0)) return 0;
    return RoughnessFade > 0 ? 1 - smoothstep(max(0, MaxRoughness - RoughnessFade), MaxRoughness, surface.w) : 1;
}

[numthreads(8, 8, 1)]
void PrepareCS(uint3 thread : SV_DispatchThreadID)
{
    uint2 size, pixel = thread.xy;
    Roughness.GetDimensions(size.x, size.y);
    if (any(pixel >= size)) return;
    float roughness = clamp(NormalRoughness[pixel].w, 0.045f, 1);
    float weight = ReflectionWeight(pixel);
    Roughness[pixel] = weight > 0 ? roughness * roughness : 1;
    MotionVectors[pixel] = Velocity[pixel] * float2(0.5f, -0.5f) + JitterDeltaUV;
    float3 fallback = 0;
    if (weight > 0)
    {
        float3 view = SafeNormalize(Camera.CameraLocation - ReconstructPosition(pixel, size));
        float3 normal = SafeNormalize(NormalRoughness[pixel].xyz);
        fallback = PrefilteredEnvironment.SampleLevel(samLinearClamp, reflect(-view, normal), roughness * 7).rgb;
    }
    FallbackOutput[pixel] = float4(fallback, 1);
}

[numthreads(8, 8, 1)]
void ComposeCS(uint3 thread : SV_DispatchThreadID)
{
    uint2 size, pixel = thread.xy;
    SceneColor.GetDimensions(size.x, size.y);
    if (any(pixel >= size)) return;
    float4 scene = InputColor[pixel];
    float weight = ReflectionWeight(pixel);
    if (weight > 0)
    {
        float3 view = SafeNormalize(Camera.CameraLocation - ReconstructPosition(pixel, size));
        float3 normal = SafeNormalize(NormalRoughness[pixel].xyz);
        float roughness = clamp(NormalRoughness[pixel].w, 0.045f, 1);
        float nDotV = saturate(dot(normal, view));
        float2 brdf = EnvironmentBRDF.SampleLevel(samLinearClamp, float2(nDotV, roughness), 0).rg;
        float3 specularFactor = F0Baked[pixel].rgb * brdf.x + brdf.y;
        float visibility = HasAmbientOcclusion ? AmbientOcclusion[pixel] : 1;
        float specularVisibility = saturate(pow(nDotV + visibility, exp2(-16 * roughness * roughness - 1)) - 1 + visibility);
        // Remove the original specular IBL even when environment fallback is disabled.
        scene.rgb = min(max(0, scene.rgb + Intensity * weight * specularVisibility
            * (Reflections[pixel].rgb - Fallback[pixel].rgb) * specularFactor), 65504.0f);
    }
    SceneColor[pixel] = scene;
}
