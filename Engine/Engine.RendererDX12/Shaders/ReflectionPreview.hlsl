#include "CBufferStructures.hlsl"

cbuffer PreviewConstants : register(b0) { uint PreviewMask; };
ConstantBuffer<CameraCB> CBCamera : register(b1);
TextureCubeArray<float4> ProbeRadiance : register(t0);
struct ProbeData
{
    float3 Position; float Intensity;
    float3 BoxMin; float BlendDistance;
    float3 BoxMax; float MaxMip;
    float MinMip;
};
StructuredBuffer<ProbeData> Probes : register(t1);
SamplerState LinearClamp : register(s3);

struct PSInput { float4 Position : SV_POSITION; float2 TexCoord : TEXCOORD; };
struct PSOutput { float4 Color : SV_TARGET; float Depth : SV_DEPTH; };

PSOutput PS(PSInput input)
{
    float2 ndc = input.TexCoord * float2(2, -2) + float2(-1, 1);
    float4 nearWorld = mul(float4(ndc, 1, 1), CBCamera.InvViewProj);
    float3 ray = normalize(nearWorld.xyz / nearWorld.w - CBCamera.CameraLocation);
    float closest = 3.402823466e+38f;
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
    if (selected == 32) discard;
    ProbeData probe = Probes[selected];
    float3 normal = normalize(CBCamera.CameraLocation + ray * closest - probe.Position);
    float3 reflection = reflect(ray, normal);
    // The exact published slice and its first populated mip show this probe's capture.
    PSOutput output;
    output.Color = float4(ProbeRadiance.SampleLevel(LinearClamp, float4(reflection, selected), probe.MinMip).rgb, 1);
    float4 clip = mul(float4(CBCamera.CameraLocation + ray * closest, 1), CBCamera.ViewProj);
    output.Depth = clip.z / clip.w;
    // shortcut: preview spheres reuse scene motion; write sphere motion if temporal artifacts require it.
    return output;
}
