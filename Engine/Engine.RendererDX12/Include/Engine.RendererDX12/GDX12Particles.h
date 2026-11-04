#pragma once
#include <cstdint>
#include "Engine.RendererDX12/D3DHelpers.h"
#include "Engine.RendererDX12/GDX12Material.h"
#include <array>

enum class ParticleBlend : std::uint8_t { WeightedAlpha, Additive, SortedAlpha };
enum class ParticleShape : std::uint8_t { Point, Box, Sphere };
enum class ParticleGeometry : std::uint8_t { Billboard, Firefly };
enum class ParticleSimulation : std::uint8_t { AnalyticDrag, SemiImplicitEuler, Count };

struct GDX12ParticleMeshVertex
{
    Vector3 Position;
    Vector3 Normal;
    Vector4 Color; // RGB material tint, W emissive fraction.
    float Wing;
};
inline constexpr UINT FireflyParticleVertexCount = 120;
static_assert(FireflyParticleVertexCount <= 600);
const std::array<GDX12ParticleMeshVertex, FireflyParticleVertexCount>& GetFireflyParticleMesh();

struct GDX12ParticleEmitterSettings
{
    UINT MaxParticles = 0; // Zero inherits the quality budget.
    UINT Substeps = 0;
    UINT EdgeSamples = 0;
    float MinPixelSize = -1.0f;
    float EmissionRate = 100.0f;
    float LifetimeMin = 1.0f;
    float LifetimeMax = 2.0f;
    float SpeedMin = 20.0f;
    float SpeedMax = 100.0f;
    Vector3 Direction = Vector3(0, 1, 0);
    float ConeAngleDegrees = 30.0f;
    ParticleShape Shape = ParticleShape::Point;
    Vector3 ShapeExtents = Vector3(10.0f);
    ParticleSimulation Simulation = ParticleSimulation::AnalyticDrag;
    bool VolumeEnabled = false;
    Vector3 VolumeHalfExtents = Vector3(100.0f); // Emitter-local cm; contains particle centres.
    Vector3 Acceleration = Vector3(0, -98.1f, 0);
    Vector3 Wind = Vector3::Zero;
    float Drag = 0.0f;
    float SizeStart = 10.0f;
    float SizeEnd = 0.0f;
    float SizeVariation = 0.25f;
    float RenderScale = 1.0f;
    Vector4 ColorStart = Vector4(1, 1, 1, 1);
    Vector4 ColorEnd = Vector4(1, 1, 1, 0);
    float AngularVelocity = 0.0f;
    float SoftDistance = 10.0f;
    float MaxDistance = 100000.0f;
    float MaxDeltaTime = 0.1f;
    float TimeScale = 1.0f;
    UINT Seed = 1;
    ParticleBlend Blend = ParticleBlend::WeightedAlpha;
    ParticleGeometry Geometry = ParticleGeometry::Billboard;
    GPUTexture* Texture = nullptr; // RenderModule-owned color texture; nullptr uses an analytic disc.
    UINT AtlasColumns = 1;
    UINT AtlasRows = 1;
    float FramesPerSecond = 0.0f; // Zero plays the entire atlas once over the lifetime.
    bool DepthCollision = false;
    float CollisionRadius = 1.5f; // World cm, independent of billboard size.
    float CollisionRestitution = 0.45f;
    float CollisionThickness = 10.0f; // Maximum initial penetration in cm.
};

struct GDX12ParticleSettings
{
    bool Enabled;
    int Quality;
};

GDX12ParticleSettings GetParticleSettings();
bool IsValidParticleEmitterSettings(const GDX12ParticleEmitterSettings& settings);
GDX12ParticleEmitterSettings ResolveParticleEmitterSettings(const GDX12ParticleEmitterSettings& author, int quality);

struct GDX12ParticleEmitter : GDX12ParticleEmitterSettings
{
    uint64_t Id = 0;
    uint64_t ResetRevision = 0;
    uint64_t BurstTotal = 0;
    Matrix World = Matrix::Identity;
    float DeltaTime = 0.0f;
    bool Paused = false;
};
