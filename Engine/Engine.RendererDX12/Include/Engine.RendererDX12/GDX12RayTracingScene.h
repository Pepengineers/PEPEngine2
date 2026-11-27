#pragma once

#include "Engine.RendererDX12/GDX12AccelerationStructure.h"
#include "Engine.RendererDX12/GDX12UploadBuffer.h"
#include "Engine.Core/AssetHandles.h"

#include <map>

class GDX12DeviceResources;

// One device-local TLAS for all worlds
class GDX12RayTracingScene
{
public:
    explicit GDX12RayTracingScene(GDX12DeviceResources& resources);

    // Update a dirty instance, its ID is the first submesh index in InstanceCache.
    void SetInstance(UINT instanceID, Engine::Core::MeshHandle mesh, const Matrix& world);
    void RemoveInstance(UINT instanceID);

    void PrepareGeometry();
    bool Build(GDX12CommandList* commands, UINT frameSlot);
    GDX12AccelerationStructure& GetTLAS();
    GDX12Descriptor* GetTLASSRV(UINT frameSlot);
    size_t GetInstanceCount() const { return _instances.size(); }

private:
    enum class UpdateMode { None, Refit, Rebuild };
    struct Instance
    {
        Engine::Core::MeshHandle Mesh;
        D3D12_RAYTRACING_INSTANCE_DESC Description{};
    };
    struct FrameResources
    {
        std::unique_ptr<GDX12UploadBuffer<D3D12_RAYTRACING_INSTANCE_DESC>> Instances;
        std::unique_ptr<GDX12Descriptor> SRV;
        bool DescriptorDirty = true;
    };

    void RequestUpdate(UpdateMode mode);

    GDX12DeviceResources& _resources;
    GDX12AccelerationStructure _tlas;
    GDX12RayTracingScratchBuffer _scratch;
    std::map<UINT, Instance> _instances;
    std::vector<FrameResources> _frames;
    UpdateMode _pendingUpdate = UpdateMode::Rebuild;
    bool _geometryDirty = false;
};
