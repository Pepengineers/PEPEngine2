// Shared physical/optical snapshot. Row-vector matrices are uploaded transposed.
struct FluidParticle
{
    float3 Position;
    float3 Velocity;
    float3 PreviousPosition;
};
cbuffer FluidParameters : register(b1)
{
    float4x4 ContainerToWorld;
    float4x4 WorldToContainer;
    float3 HalfExtent; float Radius;
    float3 Gravity; float H;
    float Mass; float RestDensity; float Stiffness; float Viscosity;
    float MaxSpeed; float MaxAcceleration; float Restitution; float Sigma;
    float DepthRange; float EdgeDepth; float IOR; float Roughness;
    float3 Absorption; float RefractionPixels;
    float3 Scattering; float MaxThickness;
    uint FilterRadius; uint ResetMotion; float NearStiffness; float PredictionSeconds;
    uint LocalSimulation; uint SnapshotMotion; uint2 ParameterPadding;
    float4x4 PreviousContainerToWorld;
};
