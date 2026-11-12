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
    UINT LocalSimulation, Padding[3];
};

GDX12FluidParameters MakeFluidParameters(const Matrix& container, const GDX12FluidSettings& settings);

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

    void Initialize(GDX12Device* device);
    static Buffers CreateBuffers(GDX12Device* device, GDX12DescriptorHeap* heap, const GDX12FluidInstance& instance);
    static float SafeStep(const GDX12FluidSettings& settings);
    static Steps PlanSteps(const GDX12FluidSettings& settings, float scaledElapsed);

    Head Record(GDX12CommandList& commands, Buffers& buffers, Head head,
        const GDX12FluidSettings& settings, Steps steps, D3D12_GPU_VIRTUAL_ADDRESS constants) const;

private:
    std::unique_ptr<GDX12RootSignature> _root;
    ComPtr<ID3D12PipelineState> _save, _hash, _sort, _presort, _mergeTail, _offsets, _densities, _integrate;
};
