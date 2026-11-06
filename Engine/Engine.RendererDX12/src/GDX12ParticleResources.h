#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"
#include <algorithm>
#include <cmath>

namespace ParticleResources
{
    struct Particle
    {
        XMFLOAT3 Position; float Age;
        XMFLOAT3 Previous; float Lifetime;
        XMFLOAT3 Velocity; float Rotation;
        float PreviousAge, PreviousRotation, SizeRandom;
        UINT Seed;
    };
    struct ParticleDraw
    {
        XMFLOAT3 Position; float Size;
        XMFLOAT3 Previous; float Rotation;
        XMFLOAT4 Color;
        float SoftDistance; UINT TextureIndex, Frame, Columns;
        UINT Rows, EdgeSamples; float ViewDepth, PreviousSize;
        float PreviousRotation, Age, PreviousAge; UINT Geometry;
    };
    struct EmitterConstants
    {
        XMFLOAT4X4 World;
        XMFLOAT3 Direction; float ConeCos;
        XMFLOAT3 Extents; UINT Shape;
        XMFLOAT3 Acceleration; float DeltaTime;
        XMFLOAT3 Wind; float Drag;
        XMFLOAT4 ColorStart, ColorEnd;
        XMFLOAT2 Lifetimes, Speeds, Sizes;
        float SizeVariation, RenderScale;
        float EmissionRate, AngularVelocity, SoftDistance, MaxDistance;
        float MinPixelSize, FramesPerSecond; UINT TextureIndex, Blend;
        UINT Columns, Rows, Substeps, EdgeSamples;
        UINT Seed, Paused; XMFLOAT2 RenderSize;
        UINT DepthCollision; float CollisionRadius, CollisionRestitution, CollisionThickness;
        UINT Geometry, Simulation, VolumeEnabled, Padding;
        XMFLOAT3 VolumeHalfExtents; float VolumePadding;
        XMFLOAT4X4 WorldToEmitter;
        XMFLOAT4X4 WorldToCollisionBox;
        XMFLOAT3 CollisionBoxHalfExtents; UINT CollisionBoxEnabled;
    };
    static_assert(sizeof(Particle) == 64);
    static_assert(sizeof(ParticleDraw) == 96);
    static_assert(sizeof(EmitterConstants) == 448);
    static_assert(sizeof(GDX12ParticleMeshVertex) == 44);

    template<class T>
    using Buffer = GDX12UploadBuffer<T>;

    template<class T>
    std::unique_ptr<Buffer<T>> CreateBuffer(GDX12DeviceResources* resources, UINT count)
    {
        auto buffer = std::make_unique<Buffer<T>>(resources->Device, count, EBufferType::Default, false);
        auto* heap = resources->SRV_UAV_Heap.get();
        // Keep buffers outside the material texture ranges and the small postprocess range.
        buffer->CreateSRV(heap, heap->GetAvailableIndex(TextureCube_StartIndex + TextureCube_RangeLength));
        buffer->CreateUAV(heap, heap->GetAvailableIndex(TextureCube_StartIndex + TextureCube_RangeLength));
        return buffer;
    }

    template<class T>
    void UAV(GDX12CommandList* commands, Buffer<T>& buffer)
    {
        auto& resource = buffer.GetResource();
        commands->ResourceBarrier({ resource.GetCurrentState() == D3D12_RESOURCE_STATE_UNORDERED_ACCESS ?
            resource.GetUAVBarrier() : resource.GetUnorderedAccessBarrier() });
    }

    struct Pool
    {
        UINT Capacity = 0;
        UINT ReadIndex = 0;
        uint64_t ResetRevision = 0;
        uint64_t BurstTotal = 0;
        bool Initialized = false;
        std::unique_ptr<Buffer<Particle>> Particles;
        std::unique_ptr<Buffer<UINT>> Alive[2];
        std::unique_ptr<Buffer<UINT>> Dead, Counters, Args;
        std::vector<std::unique_ptr<Buffer<EmitterConstants>>> Constants;

        Pool(GDX12DeviceResources* resources, const UINT capacity) : Capacity(capacity)
        {
            Particles = CreateBuffer<Particle>(resources, capacity);
            Alive[0] = CreateBuffer<UINT>(resources, capacity);
            Alive[1] = CreateBuffer<UINT>(resources, capacity);
            Dead = CreateBuffer<UINT>(resources, capacity);
            Counters = CreateBuffer<UINT>(resources, 8);
            Args = CreateBuffer<UINT>(resources, 9);
            for (size_t i = 0; i < resources->FrameConstants.size(); ++i)
                Constants.push_back(std::make_unique<Buffer<EmitterConstants>>(resources->Device, 1));
        }
    };

    inline std::unique_ptr<GDX12RootSignature> CreateRoot(GDX12DeviceResources* resources)
    {
        GDX12RootSignatureDesc root;
        root.VolatileSRVData = true;
        root.Constants = { 4 };
        root.NumSingleCBVSlots = 2;
        root.NumSingleSRVSlots = 4;
        root.SRVRanges.emplace_back(Texture2D_RangeLength);
        root.NumSingleUAVSlots = 12;
        root.StaticSamplers.emplace_back(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
        return std::make_unique<GDX12RootSignature>(resources->Device, root);
    }

    inline EmitterConstants MakeConstants(const GDX12ParticleEmitter& emitter, UINT width, UINT height)
    {
        EmitterConstants constants = {};
        XMStoreFloat4x4(&constants.World, XMMatrixTranspose(emitter.World));
        constants.Direction = emitter.Direction;
        constants.ConeCos = std::cos(XMConvertToRadians(emitter.ConeAngleDegrees));
        constants.Extents = emitter.ShapeExtents;
        constants.Shape = static_cast<UINT>(emitter.Shape);
        constants.Acceleration = emitter.Acceleration;
        constants.DeltaTime = emitter.Paused ? 0 : (std::min)(emitter.DeltaTime * emitter.TimeScale, emitter.MaxDeltaTime);
        constants.Wind = emitter.Wind;
        constants.Drag = emitter.Drag;
        constants.ColorStart = emitter.ColorStart;
        constants.ColorEnd = emitter.ColorEnd;
        constants.Lifetimes = XMFLOAT2(emitter.LifetimeMin, emitter.LifetimeMax);
        constants.Speeds = XMFLOAT2(emitter.SpeedMin, emitter.SpeedMax);
        constants.Sizes = XMFLOAT2(emitter.SizeStart, emitter.SizeEnd);
        constants.SizeVariation = emitter.SizeVariation;
        constants.RenderScale = emitter.RenderScale;
        constants.EmissionRate = emitter.EmissionRate;
        constants.AngularVelocity = emitter.AngularVelocity;
        constants.SoftDistance = emitter.SoftDistance;
        constants.MaxDistance = emitter.MaxDistance;
        constants.MinPixelSize = emitter.MinPixelSize;
        constants.FramesPerSecond = emitter.FramesPerSecond;
        constants.TextureIndex = UINT_MAX;
        constants.Blend = static_cast<UINT>(emitter.Blend);
        constants.Columns = emitter.AtlasColumns;
        constants.Rows = emitter.AtlasRows;
        constants.Substeps = emitter.Substeps;
        constants.EdgeSamples = emitter.EdgeSamples;
        constants.Seed = emitter.Seed;
        constants.Paused = emitter.Paused;
        constants.RenderSize = XMFLOAT2(static_cast<float>(width), static_cast<float>(height));
        constants.DepthCollision = emitter.DepthCollision;
        constants.CollisionRadius = emitter.CollisionRadius;
        constants.CollisionRestitution = emitter.CollisionRestitution;
        constants.CollisionThickness = emitter.CollisionThickness;
        constants.Geometry = static_cast<UINT>(emitter.Geometry);
        constants.Simulation = static_cast<UINT>(emitter.Simulation);
        constants.VolumeEnabled = emitter.VolumeEnabled;
        constants.VolumeHalfExtents = emitter.VolumeHalfExtents;
        XMStoreFloat4x4(&constants.WorldToEmitter,
            emitter.VolumeEnabled ? emitter.World.Invert().Transpose() : Matrix::Identity);
        const auto collisionWorld = Matrix::CreateRotationY(XMConvertToRadians(emitter.CollisionBoxYawDegrees)) *
            Matrix::CreateTranslation(emitter.CollisionBoxCenter);
        XMStoreFloat4x4(&constants.WorldToCollisionBox,
            emitter.CollisionBoxEnabled ? collisionWorld.Invert().Transpose() : Matrix::Identity);
        constants.CollisionBoxHalfExtents = emitter.CollisionBoxHalfExtents;
        constants.CollisionBoxEnabled = emitter.CollisionBoxEnabled;
        return constants;
    }
}

struct GDX12ParticleSimulation::State
{
    explicit State(GDX12DeviceResources* resources);
    bool Active = false;
    bool ResetMotion = false;
    UINT Capacity = 0;
    std::vector<GDX12ParticleEmitter> Emitters;
    std::unordered_map<uint64_t, std::unique_ptr<ParticleResources::Pool>> Pools;
    std::unique_ptr<GDX12RootSignature> Root;
    std::unordered_map<std::string, ComPtr<ID3D12PipelineState>> Compute;
    ComPtr<ID3D12CommandSignature> DispatchSignature;
};
