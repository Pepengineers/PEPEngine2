#include "Engine.RendererDX12/GDX12RayTracingScene.h"

#include "Engine.RendererDX12/GDX12CommandList.h"
#include "Engine.RendererDX12/GDX12DeviceResources.h"

#include <cstring>
#include <limits>
#include <stdexcept>

GDX12RayTracingScene::GDX12RayTracingScene(GDX12DeviceResources& resources)
    : _resources(resources), _tlas(resources.Device), _scratch(resources.Device)
{
    if (!resources.Device || !resources.GeometryBuffer || !resources.SRV_UAV_Heap || resources.FrameConstants.empty()
        || resources.Device->GetDeviceFeatures().Features.RaytracingTier() == D3D12_RAYTRACING_TIER_NOT_SUPPORTED)
    { throw std::invalid_argument("ERROR: Ray tracing scene requires initialized device resources with DXR support."); }
    _frames.resize(resources.FrameConstants.size());
}

void GDX12RayTracingScene::SetInstance(UINT instanceID, Engine::Core::MeshHandle mesh, const Matrix& world)
{
    if (instanceID > 0x00ffffffu)
    { throw std::out_of_range("ERROR: DXR InstanceID exceeds its 24-bit range."); }
    Instance instance;
    instance.Mesh = mesh;
    instance.Description.InstanceID = instanceID;
    instance.Description.InstanceMask = 0xff;
    instance.Description.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
    const auto transform = world.Transpose();
    memcpy(instance.Description.Transform, &transform, sizeof(instance.Description.Transform));
    const auto found = _instances.find(instanceID);
    if (found != _instances.end() && found->second.Mesh == mesh
        && memcmp(&found->second.Description, &instance.Description, sizeof(instance.Description)) == 0)
    { return; }
    const bool geometryChanged = found == _instances.end() || found->second.Mesh != mesh;
    if (found == _instances.end() && _instances.size() >= D3D12_RAYTRACING_MAX_INSTANCES_PER_TOP_LEVEL_ACCELERATION_STRUCTURE)
    { throw std::length_error("ERROR: DXR scene exceeds the maximum instance count."); }
    _instances.insert_or_assign(instanceID, instance);
    _geometryDirty |= geometryChanged;
    RequestUpdate(geometryChanged ? UpdateMode::Rebuild : UpdateMode::Refit);
}

void GDX12RayTracingScene::RemoveInstance(UINT instanceID)
{
    if (_instances.erase(instanceID)) { RequestUpdate(UpdateMode::Rebuild); }
}

void GDX12RayTracingScene::RequestUpdate(UpdateMode mode)
{
    if (_pendingUpdate < mode) { _pendingUpdate = mode; }
}

void GDX12RayTracingScene::PrepareGeometry()
{
    if (!_geometryDirty) { return; }
    std::vector<Engine::Core::MeshHandle> meshes;
    meshes.reserve(_instances.size());
    for (const auto& [id, instance] : _instances) { meshes.push_back(instance.Mesh); }
    _resources.GeometryBuffer->BuildBLAS(meshes);
    _geometryDirty = false;
}

bool GDX12RayTracingScene::Build(GDX12CommandList* commands, UINT frameSlot)
{
    if (!commands || commands->GetDevice() != _resources.Device
        || (commands->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT && commands->GetType() != D3D12_COMMAND_LIST_TYPE_COMPUTE)
        || commands->FenceValue != (std::numeric_limits<UINT64>::max)())
    { throw std::invalid_argument("ERROR: TLAS requires a recording DIRECT or COMPUTE list on the scene's GPU."); }
    if (_geometryDirty) { throw std::logic_error("ERROR: Call PrepareGeometry before recording TLAS work."); }
    _frames.resize(_resources.FrameConstants.size());
    auto& frame = _frames.at(frameSlot);
    const bool needsBuild = _pendingUpdate != UpdateMode::None;
    if (needsBuild)
    {
        if (!frame.Instances)
            frame.Instances = std::make_unique<GDX12UploadBuffer<D3D12_RAYTRACING_INSTANCE_DESC>>(
                _resources.Device, 0, EBufferType::Upload, false);
        const auto count = static_cast<UINT>(_instances.size());
        frame.Instances->Resize(count);
        UINT index = 0;
        for (const auto& [id, instance] : _instances)
        {
            auto description = instance.Description;
            const auto* mesh = _resources.GeometryBuffer->GetGPUMeshByHandle(instance.Mesh);
            if (!mesh || !mesh->BLAS) { throw std::logic_error("ERROR: TLAS instance has no prepared BLAS."); }
            description.AccelerationStructure = mesh->BLAS->GetGPUVirtualAddress();
            commands->KeepResourceAlive(mesh->BLAS->GetResource().D3DResource.Get());
            frame.Instances->CopyData(index++, description);
        }
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
        inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        inputs.NumDescs = count;
        inputs.InstanceDescs = count ? frame.Instances->GetElementAddress(0) : 0;
        inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE
            | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        if (_pendingUpdate == UpdateMode::Refit)
            inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
        const auto previousAddress = _tlas.GetResource().D3DResource ? _tlas.GetGPUVirtualAddress() : 0;
        commands->KeepResourceAlive(frame.Instances->GetResource().D3DResource.Get());
        commands->BeginPixEvent(_pendingUpdate == UpdateMode::Rebuild ? "Build TLAS" : "Refit TLAS", Colors::Orange);
        _tlas.Build(commands, inputs, _scratch);
        commands->EndPixEvent();
        if (previousAddress != _tlas.GetGPUVirtualAddress())
            for (auto& slot : _frames) { slot.DescriptorDirty = true; }
        _pendingUpdate = UpdateMode::None;
    }
    commands->KeepResourceAlive(_tlas.GetResource().D3DResource.Get());
    if (frame.DescriptorDirty)
    {
        if (!frame.SRV) { frame.SRV = std::make_unique<GDX12Descriptor>(); }
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
        view.RaytracingAccelerationStructure.Location = _tlas.GetGPUVirtualAddress();
        auto* heap = _resources.SRV_UAV_Heap.get();
        const auto heapIndex = frame.SRV->HeapIndex == UINT_MAX ? heap->GetAvailableIndex(RendererResources_StartIndex) : frame.SRV->HeapIndex;
        frame.SRV->InitAsSRV(nullptr, &view, heap, heapIndex);
        frame.DescriptorDirty = false;
    }
    return needsBuild;
}

GDX12AccelerationStructure& GDX12RayTracingScene::GetTLAS()
{
    if (_pendingUpdate != UpdateMode::None)
    { throw std::logic_error("ERROR: Build TLAS before accessing it."); }
    return _tlas;
}

GDX12Descriptor* GDX12RayTracingScene::GetTLASSRV(UINT frameSlot)
{
    GetTLAS();
    auto& frame = _frames.at(frameSlot);
    if (frame.DescriptorDirty) { throw std::logic_error("ERROR: Call Build for this frame slot before accessing its TLAS SRV."); }
    return frame.SRV.get();
}
