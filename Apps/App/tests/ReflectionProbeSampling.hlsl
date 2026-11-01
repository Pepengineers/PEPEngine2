SamplerState samLinearClamp : register(s3);
#define REFLECTION_PROBE_BINDINGS_DEFERRED 1
#include "../../../Engine/Engine.RendererDX12/Shaders/ReflectionProbes.hlsli"
cbuffer Query : register(b0)
{
    float3 Position; float Roughness;
    float3 Direction; float Padding;
    float3 Global; float Padding2;
};
RWStructuredBuffer<float4> Result : register(u0);
[numthreads(1, 1, 1)]
void CS()
{
    Result[0] = float4(SampleReflectionRadiance(Position, Direction, Roughness, Global), 1);
}
