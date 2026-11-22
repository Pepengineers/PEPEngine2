// Row-vector matrices are uploaded transposed.
struct FluidParticle
{
    float3 Position;
    float3 Velocity;
};
cbuffer FluidParameters : register(b1)
{
    float4x4 ContainerToWorld;
    float4x4 WorldToContainer;
    float3 HalfExtent; float Radius;
    float3 Gravity; float H;
    float Mass; float RestDensity; float Stiffness; float Viscosity;
    float MaxSpeed; float MaxAcceleration; float Restitution; float NearStiffness;
    float PredictionSeconds; uint LocalSimulation; uint2 ParameterPadding;
};
