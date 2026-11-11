#pragma once

#include "Rendering/Water/GDX12Fluid.h"
#include "Engine.RendererDX12/GDX12RootSignature.h"
#include "Engine.RendererDX12/GDX12UploadBuffer.h"

class GDX12CommandList;

struct GDX12FluidParameters
{
    XMFLOAT4X4 ContainerToWorld, WorldToContainer;
    XMFLOAT3 HalfExtent; float Radius;
    XMFLOAT3 Gravity; float H;
    float Mass, RestDensity, Stiffness, Viscosity;
    float MaxSpeed, MaxAcceleration, Restitution, Sigma;
    float DepthRange, EdgeDepth, IOR, Roughness;
    XMFLOAT3 Absorption; float RefractionPixels;
    XMFLOAT3 Scattering; float MaxThickness;
    UINT FilterRadius, ResetMotion;
    float NearStiffness, PredictionSeconds;
    UINT LocalSimulation, SnapshotMotion, Padding[2];
    XMFLOAT4X4 PreviousContainerToWorld;
};

GDX12FluidParameters MakeFluidParameters(const Matrix& container, const GDX12FluidSettings& settings);

class GDX12FluidProducerInput
{
public:
    GDX12FluidProducerInput() = default;
    GDX12FluidProducerInput(const GDX12FluidInstance& instance, int quality);

    // Use the clock tick's Seconds for queued work.
    const GDX12FluidInstance& Instance() const { return _instance; }
    const GDX12FluidSettings& Settings() const { return _settings; }
    double Elapsed() const;
    double LogicalTick() const;
    friend bool operator==(const GDX12FluidProducerInput& a, const GDX12FluidProducerInput& b);

private:
    GDX12FluidInstance _instance;
    GDX12FluidSettings _settings;
    int _quality = 0;
};

class GDX12FluidSolver
{
public:
    struct Particle { XMFLOAT3 Position, Velocity, PreviousPosition; };
    struct Buffers
    {
        std::unique_ptr<GDX12UploadBuffer<Particle>> Particles[2], SeedUpload;
        std::unique_ptr<GDX12UploadBuffer<XMFLOAT2>> Density;
        std::unique_ptr<GDX12UploadBuffer<XMUINT2>> Keys;
        std::unique_ptr<GDX12UploadBuffer<UINT>> Offsets;
        UINT Count = 0, Capacity = 0;
    };
    struct Head
    {
        UINT Current = 0;
        bool SeedPending = true;
        float WaveTime = 0;
    };
    struct Steps
    {
        UINT Count = 0;
        float Dt = 0;
    };
    enum class Boundary { LegacyTracked, PlacedUav };

    void Initialize(GDX12Device* device);
    static Buffers CreateBuffers(GDX12Device* device, GDX12DescriptorHeap* heap, const GDX12FluidInstance& instance);
    static float SafeStep(const GDX12FluidSettings& settings);
    static Steps PlanSteps(const GDX12FluidSettings& settings, float scaledElapsed);

    // Placed recording enters/exits all solver default buffers in declared UAV state.
    // The caller owns the descriptor heap, constant address and the returned staged head.
    Head Record(GDX12CommandList& commands, Buffers& buffers, Head head,
        const GDX12FluidSettings& settings, Steps steps, D3D12_GPU_VIRTUAL_ADDRESS constants, Boundary boundary) const;
    // The placed publish pass declares the chosen final particle buffer COPY_SOURCE and its slot COPY_DEST.
    static void RecordPublish(GDX12CommandList& commands, ID3D12Resource& particles,
        ID3D12Resource& destination, UINT count);

private:
    std::unique_ptr<GDX12RootSignature> _root;
    ComPtr<ID3D12PipelineState> _save, _hash, _sort, _presort, _mergeTail, _offsets, _densities, _integrate;
};
