#include "CBufferStructures.hlsl"
ConstantBuffer<CameraCB> Camera : register(b0);
Texture2D<float> Depth : register(t0);
Texture2D<float4> NormalRoughness : register(t1);
TextureCube<float4> PrefilteredEnvironment : register(t2);
Texture2D<float4> F0Baked : register(t3);
SamplerState samLinearClamp : register(s3);
#define PROBE_GLOBALS_REGISTER b1
#define PROBE_DATA_REGISTER t4
#define PROBE_CUBES_REGISTER t5
#include "ProbeIBL.hlsli"
RWTexture2D<float4> Result : register(u0);
[numthreads(8,8,1)]
void CS(uint2 pixel : SV_DispatchThreadID)
{
    uint width, height; Result.GetDimensions(width, height);
    if (any(pixel >= uint2(width, height))) return;
    float4 normal = NormalRoughness[pixel];
    float3 fallback = -1;
    float depth = Depth[pixel];
    bool traceable = depth > 0 && F0Baked[pixel].a <= 0.5;
#if defined(PROBE_LIGHTING)
    if (traceable && ReflectionProbeCount > 0)
    {
        float2 ndc = (float2(pixel) + 0.5) / uint2(width, height) * float2(2,-2) + float2(-1,1);
        float4 world = mul(float4(ndc, depth, 1), Camera.InvViewProj);
        float3 position = world.xyz / world.w;
        float3 view = Camera.CameraLocation - position;
        view *= rsqrt(max(dot(view, view), 1e-12));
        float3 n = normal.xyz * rsqrt(max(dot(normal.xyz, normal.xyz), 1e-12));
        float3 direction = reflect(-view, n);
        float weight;
        float3 local = ProbeSpecularRadiance(position, direction, normal.w, weight);
        if (weight > 0)
            fallback = (local + saturate(1-weight) * GlobalSpecularRadiance(direction, normal.w)) / max(1,weight);
    }
#endif
    Result[pixel] = float4(fallback, traceable ? normal.w : 1);
}
