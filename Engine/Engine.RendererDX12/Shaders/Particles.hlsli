#include "CBufferStructures.hlsl"

struct Particle
{
    float3 Position;
    float Age;
    float3 Previous;
    float Lifetime;
    float3 Velocity;
    float Rotation;
    float PreviousAge;
    float PreviousRotation;
    float SizeRandom;
    uint Seed;
    uint2 Identity;
};

struct ParticleDraw
{
    float3 Position;
    float Size;
    float3 Previous;
    float Rotation;
    float4 Color;
    float SoftDistance;
    uint TextureIndex;
    uint Frame;
    uint Columns;
    uint Rows;
    uint EdgeSamples;
    float ViewDepth;
    float PreviousSize;
    float PreviousRotation;
    float Age;
    float PreviousAge;
    uint Geometry;
};

static const uint ParticleGeometryFirefly = 1;
static const uint ParticleGeometryWaterDrop = 2;

cbuffer Parameters : register(b0)
{
    uint Capacity; // Draw: index-list offset.
    uint BurstCount; // Draw: blend mode (0 weighted, 1 additive, 2 sorted).
    uint TotalCapacity;
    uint ResetMotion;
};
ConstantBuffer<CameraCB> Camera : register(b1);

cbuffer Emitter : register(b2)
{
    float4x4 EmitterWorld;
    float3 Direction;
    float ConeCos;
    float3 Extents;
    uint Shape;
    float3 Acceleration;
    float DeltaTime;
    float3 Wind;
    float Drag;
    float4 ColorStart;
    float4 ColorEnd;
    float2 Lifetimes;
    float2 Speeds;
    float2 Sizes;
    float SizeVariation;
    float RenderScale;
    float EmissionRate;
    float AngularVelocity;
    float SoftDistance;
    float MaxDistance;
    float MinPixelSize;
    float FramesPerSecond;
    uint TextureIndex;
    uint BlendMode;
    uint Columns;
    uint Rows;
    uint Substeps;
    uint EdgeSamples;
    uint Seed;
    uint Paused;
    float2 RenderSize;
    uint DepthCollision;
    float CollisionRadius;
    float CollisionRestitution;
    float CollisionThickness;
    uint Geometry;
    uint Simulation;
    uint VolumeEnabled;
    float CollisionFriction;
    float3 VolumeHalfExtents;
    float VolumePadding;
    float4x4 WorldToEmitter;
    float4x4 WorldToCollisionBox;
    float3 CollisionBoxHalfExtents;
    uint CollisionBoxEnabled;
    float4 InteractionArea;
    uint InteractionResolution; float InteractionFloor; float2 InteractionPadding;
};
