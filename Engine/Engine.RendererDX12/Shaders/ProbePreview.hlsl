#include "CBufferStructures.hlsl"

cbuffer PreviewConstants : register(b0)
{
    uint InstanceIndex;
    uint CubeIndex;
    uint ResetMotion;
};
ConstantBuffer<CameraCB> CBCamera : register(b1);
StructuredBuffer<Transform> TransformCache : register(t0);
StructuredBuffer<InstanceData> InstanceCache : register(t1);
struct ProbeData
{
    float3 Position; float Intensity;
    float3 BoxMin; float BlendDistance;
    float3 BoxMax; float MaxMip;
    float MinMip;
};
StructuredBuffer<ProbeData> Probes : register(t2);
TextureCubeArray<float4> Radiance : register(t3);
SamplerState LinearClamp : register(s3);
struct VertexInput
{
    float3 Position : POSITION;
    float2 UV : TEXCOORD;
    float3 Normal : NORMAL;
    float3 Tangent : TANGENT;
    float3 PreviousPosition : PREVPOSITION;
};
struct VertexOutput
{
    float4 Position : SV_POSITION;
    float3 WorldPosition : TEXCOORD0;
    float3 Normal : TEXCOORD1;
    float4 Current : TEXCOORD2;
    float4 Previous : TEXCOORD3;
};
VertexOutput VS(VertexInput input)
{
    Transform transform = TransformCache[InstanceCache[InstanceIndex].TransformIndex];
    float4 world = mul(float4(input.Position, 1), transform.World);
    VertexOutput output;
    output.Position = mul(world, CBCamera.ViewProj);
    output.WorldPosition = world.xyz;
    output.Normal = normalize(mul(input.Normal, (float3x3)transform.World));
    output.Current = mul(world, CBCamera.ViewProjNoJitter);
    output.Previous = mul(mul(float4(input.PreviousPosition, 1), transform.PrevWorld), CBCamera.PrevViewProjNoJitter);
    return output;
}
struct PixelOutput
{
    float4 Color : SV_TARGET0;
    float2 Motion : SV_TARGET1;
};
PixelOutput PS(VertexOutput input)
{
    PixelOutput output;
    output.Color = float4(0.18f, 0.18f, 0.18f, 1);
    uint count, stride;
    Probes.GetDimensions(count, stride);
    if (CubeIndex < count)
    {
        ProbeData probe = Probes[CubeIndex];
        // Zero extent denotes a probe with no completed publication.
        if (all(probe.BoxMax > probe.BoxMin))
        {
            float3 direction = reflect(normalize(input.WorldPosition - CBCamera.CameraLocation), normalize(input.Normal));
            output.Color.rgb = Radiance.SampleLevel(LinearClamp, float4(direction, CubeIndex), probe.MinMip).rgb;
        }
    }
    output.Motion = ResetMotion != 0 || input.Previous.w <= 0.00001f || input.Current.w <= 0.00001f ? 0 :
        input.Previous.xy / input.Previous.w - input.Current.xy / input.Current.w;
    return output;
}
