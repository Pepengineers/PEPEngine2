#include "CBufferStructures.hlsl"

cbuffer PreviewConstants : register(b0) { uint PreviewMask; };
ConstantBuffer<CameraCB> CBCamera : register(b1);
Texture2D<float> Depth : register(t0);
TextureCubeArray<float4> ProbeRadiance : register(t1);
struct ProbeData
{
    float3 Position; float Intensity;
    float3 BoxMin; float BlendDistance;
    float3 BoxMax; float MaxMip;
    float MinMip;
};
StructuredBuffer<ProbeData> Probes : register(t2);
RWTexture2D<float4> Color : register(u0);
SamplerState LinearClamp : register(s3);

[numthreads(8, 8, 1)]
void CS(uint3 id : SV_DispatchThreadID)
{
    uint width, height;
    Color.GetDimensions(width, height);
    if (any(id.xy >= uint2(width, height))) return;
    float2 ndc = (float2(id.xy) + 0.5f) / float2(width, height) * float2(2, -2) + float2(-1, 1);
    float4 nearWorld = mul(float4(ndc, 1, 1), CBCamera.InvViewProj);
    float3 ray = normalize(nearWorld.xyz / nearWorld.w - CBCamera.CameraLocation);
    float closest = 3.402823466e+38f;
    float depth = Depth[id.xy];
    if (depth > 0)
    {
        float4 world = mul(float4(ndc, depth, 1), CBCamera.InvViewProj);
        closest = dot(world.xyz / world.w - CBCamera.CameraLocation, ray);
    }
    uint selected = 32;
    uint remaining = PreviewMask;
    while (remaining)
    {
        uint index = firstbitlow(remaining);
        remaining &= remaining - 1;
        ProbeData probe = Probes[index];
        if (probe.Intensity <= 0) continue; // No complete published capture yet.
        float3 extent = (probe.BoxMax - probe.BoxMin) * 0.5f;
        float radius = 0.8f * min(extent.x, min(extent.y, extent.z));
        float3 offset = CBCamera.CameraLocation - probe.Position;
        float b = dot(offset, ray);
        float discriminant = b * b - dot(offset, offset) + radius * radius;
        if (discriminant < 0) continue;
        float root = sqrt(discriminant);
        float distance = -b - root;
        if (distance <= 0) distance = -b + root;
        if (distance <= 0 || distance >= closest) continue;
        float4 clip = mul(float4(CBCamera.CameraLocation + ray * distance, 1), CBCamera.ViewProj);
        if (clip.w <= 0 || clip.z < 0 || clip.z > clip.w) continue;
        closest = distance;
        selected = index;
    }
    if (selected == 32) return;
    ProbeData probe = Probes[selected];
    float3 normal = normalize(CBCamera.CameraLocation + ray * closest - probe.Position);
    float3 reflection = reflect(ray, normal);
    // The exact published slice and its first populated mip show this probe's capture.
    Color[id.xy] = float4(ProbeRadiance.SampleLevel(LinearClamp, float4(reflection, selected), probe.MinMip).rgb, 1);
    // shortcut: debug spheres do not write depth/motion, use geometry if previews must participate in post effects.
}
