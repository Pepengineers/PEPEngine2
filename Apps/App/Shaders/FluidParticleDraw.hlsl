// Ported from Multi-GPU-Render-DX12/MGPU-FluidSimulation/Shaders/FluidParticleDraw.hlsl.
#include "CBufferStructures.hlsl"
#include "FluidParameters.hlsli"
ConstantBuffer<CameraCB> Camera : register(b2);
StructuredBuffer<FluidParticle> Particles : register(t0);

struct VertexOutput
{
    float4 Position : SV_POSITION;
    nointerpolation float3 Color : TEXCOORD0;
};

VertexOutput VS(uint vertex : SV_VertexID, uint instance : SV_InstanceID)
{
    static const float2 corners[4] = {
        float2(-0.5f, -0.5f), float2(0.5f, -0.5f),
        float2(-0.5f, 0.5f), float2(0.5f, 0.5f)
    };
    FluidParticle particle = Particles[instance];
    float3 center = LocalSimulation != 0 ? mul(float4(particle.Position, 1), ContainerToWorld).xyz : particle.Position;
    float3 right = float3(Camera.View._11, Camera.View._21, Camera.View._31);
    float3 up = float3(Camera.View._12, Camera.View._22, Camera.View._32);
    float3 position = center + (right * corners[vertex].x + up * corners[vertex].y) * (2 * Radius);
    VertexOutput output;
    output.Position = mul(float4(position, 1), Camera.ViewProj);
    output.Color = lerp(float3(0, 0, 1), float3(1, 0, 0), saturate(length(particle.Velocity) / 10.0f));
    return output;
}

float4 PS(VertexOutput input) : SV_TARGET { return float4(input.Color, 1); }
