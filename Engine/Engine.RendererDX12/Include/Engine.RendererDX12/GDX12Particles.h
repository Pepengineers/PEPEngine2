#pragma once
#include <cstdint>
#include "Engine.RendererDX12/D3DHelpers.h"
#include "Engine.RendererDX12/GDX12Material.h"
#include <array>
#include <memory>
#include <vector>

enum class ParticleBlend : std::uint8_t { WeightedAlpha, Additive, SortedAlpha };
enum class ParticleShape : std::uint8_t { Point, Box, Sphere };
enum class ParticleGeometry : std::uint8_t { Billboard, Firefly, WaterDrop };
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
    bool InteractionEnabled = false;
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
    float CollisionFriction = 0; // Coulomb friction coefficient; zero preserves tangential speed.
    float CollisionThickness = 10.0f; // Maximum initial penetration in cm.
    bool CollisionBoxEnabled = false;
    Vector3 CollisionBoxCenter = Vector3::Zero; // World-space solid obstacle, independent of camera depth.
    Vector3 CollisionBoxHalfExtents = Vector3::One;
    float CollisionBoxYawDegrees = 0;
};

struct GDX12ParticleSettings
{
    bool Enabled;
    int Quality;
    bool DrawEnabled;
    bool SimulationEnabled;
    bool AsyncCompute;
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

class GDX12DeviceResources;
class GDX12CommandList;
class GDX12Texture;
class GDX12Resource;
class RenderPipelineCommonData;

// Borrowed until GPU frame retirement. Cells: direction XZ, presence [0,1], world top Y.
struct GDX12ParticleInteractionField
{
    GDX12Resource* Resource = nullptr;
    D3D12_GPU_DESCRIPTOR_HANDLE SRV = {};
    XMFLOAT4 Area = {}; // world XZ centre and half extents
    UINT Resolution = 0;
    float FloorCm = 0;
};

class GDX12ParticleSimulation
{
public:
    GDX12ParticleSimulation();
    ~GDX12ParticleSimulation();
    void BeginFrame(GDX12DeviceResources* resources, RenderPipelineCommonData& common,
        const GDX12ParticleSettings* capturedSettings = nullptr);
    void InitializePools(GDX12CommandList* commands, GDX12DeviceResources* resources);
    std::vector<GDX12Resource*> GetPoolResources() const;
    void SetInteractionField(GDX12ParticleInteractionField field) { _interaction = field; }
    GDX12Resource* GetInteractionResource() const { return _interaction.Resource; }
    GDX12ParticleInteractionField GetInteractionField() const { return _interaction; }
    void Execute(GDX12CommandList* commands, GDX12DeviceResources* resources,
        RenderPipelineCommonData& common, GDX12Texture* opaqueDepth,
        D3D12_GPU_VIRTUAL_ADDRESS camera = 0, bool preserveTime = false);

    struct Checkpoint
    {
        struct Pool
        {
            UINT Capacity, ReadIndex;
            uint64_t ResetRevision, BurstTotal;
            bool Initialized;
            std::array<std::shared_ptr<GDX12Resource>, 6> Buffers;
        };
        std::vector<GDX12ParticleEmitter> Emitters;
        std::vector<Pool> Pools;
        bool Active = false, ResetMotion = false;
        UINT Capacity = 0;
        UINT64 Bytes = 0;
    };
    Checkpoint CaptureCheckpoint() const;
    std::vector<std::shared_ptr<GDX12Resource>> GetConstantResources(UINT frame) const;
    void RestoreCheckpointMetadata(const Checkpoint& checkpoint);
    static void RecordCheckpoint(GDX12CommandList& commands, const Checkpoint& checkpoint,
        GDX12Resource& payload, bool restore);
    void RecordDisplayHistory(GDX12CommandList& commands, GDX12DeviceResources& resources,
        uint64_t emitter, D3D12_GPU_DESCRIPTOR_HANDLE previous, bool valid);

private:
    friend class GDX12ParticlesPass;
    struct State;
    std::unique_ptr<State> _state;
    GDX12ParticleInteractionField _interaction;
};
