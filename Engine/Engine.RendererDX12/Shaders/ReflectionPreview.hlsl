#include "CBufferStructures.hlsl"
ConstantBuffer<CameraCB> CBCamera : register(b2);
StructuredBuffer<Material> Materials : register(t0);
TextureCubeArray<float4> ProbeRadiance : register(t3);
struct ProbeData
{
    float3 Position; float Intensity;
    float3 BoxMin; float BlendDistance;
    float3 BoxMax; float MaxMip;
    float MinMip;
};
StructuredBuffer<ProbeData> Probes : register(t4);
SamplerState LinearClamp : register(s3);

struct PSInput
{
    float4 Position : SV_POSITION;
    float4 Previous : TEXCOORD2;
    float3 WorldPosition : POSITION;
    float4 Current : POSITION2;
    float2 TexCoord : TEXCOORD;
    float3 Normal : NORMAL;
    float3 Tangent : TANGENT;
    uint MaterialIndex : TEXCOORD1;
};
struct PSOutput
{
    float4 Color : SV_TARGET0;
    float2 Motion : SV_TARGET1;
    float4 Normal : SV_TARGET2;
    float4 F0Baked : SV_TARGET3;
};
PSOutput PS(PSInput input)
{
    uint index = Materials[input.MaterialIndex].ReflectionProbePreview - 1;
    uint count, stride;
    Probes.GetDimensions(count, stride);
    float3 n = normalize(input.Normal);
    float3 view = normalize(CBCamera.CameraLocation - input.WorldPosition);
    float3 radiance = 0;
    if (index < count && Probes[index].Intensity > 0)
        radiance = ProbeRadiance.SampleLevel(LinearClamp, float4(reflect(-view, n), index), Probes[index].MinMip).rgb;
    PSOutput output;
    output.Color = float4(radiance, 1);
    output.Motion = input.Previous.w > 0.00001f ? input.Previous.xy / input.Previous.w - input.Current.xy / input.Current.w : 0;
    output.Normal = float4(n, 1);
    output.F0Baked = float4(0, 0, 0, 1); // Preserve the unlit result through the subsequent SSR resolve.
    return output;
}
