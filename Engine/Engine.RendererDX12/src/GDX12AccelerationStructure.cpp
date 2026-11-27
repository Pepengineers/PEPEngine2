#include "Engine.RendererDX12/GDX12AccelerationStructure.h"

#include "Engine.RendererDX12/GDX12CommandList.h"
#include "Engine.RendererDX12/GDX12Device.h"

#include <limits>
#include <stdexcept>

namespace
{
    UINT64 AlignASSize(UINT64 size)
    {
        constexpr UINT64 alignment = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT;
        if (size > (std::numeric_limits<UINT64>::max)() - (alignment - 1))
        { throw std::overflow_error("ERROR: Acceleration structure buffer size overflows alignment."); }
        return (size + alignment - 1) & ~(alignment - 1);
    }

    GDX12Resource CreateASBuffer(GDX12Device* device, UINT64 size, D3D12_RESOURCE_STATES state)
    {
        GDX12Resource resource;
        const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        const auto description = CD3DX12_RESOURCE_DESC::Buffer(AlignASSize(size), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        ThrowIfFailed(device->GetDevice()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
            &description, state, nullptr, IID_PPV_ARGS(&resource.D3DResource)));
        resource.SetCurrentState(state);
        return resource;
    }
}

GDX12RayTracingScratchBuffer::GDX12RayTracingScratchBuffer(GDX12Device* device) : _device(device)
{
    if (!device) { throw std::invalid_argument("ERROR: Ray tracing scratch buffer invalid device."); }
}

void GDX12RayTracingScratchBuffer::Reserve(UINT64 sizeInBytes)
{
    if (!sizeInBytes || (_buffer.D3DResource && _buffer.D3DResource->GetDesc().Width >= sizeInBytes)) { return; }
    _buffer = CreateASBuffer(_device, sizeInBytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

GDX12AccelerationStructure::GDX12AccelerationStructure(GDX12Device* device) : _device(device)
{
    if (!device || device->GetDeviceFeatures().Features.RaytracingTier() == D3D12_RAYTRACING_TIER_NOT_SUPPORTED)
    { throw std::invalid_argument("ERROR: Acceleration structure requires a device with DXR support."); }
}

D3D12_GPU_VIRTUAL_ADDRESS GDX12AccelerationStructure::GetGPUVirtualAddress() const
{
    if (!_buffer.D3DResource) { throw std::logic_error("ERROR: Acceleration structure has not been built."); }
    return _buffer.D3DResource->GetGPUVirtualAddress();
}

void GDX12AccelerationStructure::Build(GDX12CommandList* commands,
    const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS& inputs, GDX12RayTracingScratchBuffer& scratch)
{
    if (!commands || commands->GetDevice() != _device || scratch._device != _device)
    { throw std::invalid_argument("ERROR: Acceleration structure, scratch and command list must belong to the same GPU."); }
    if ((commands->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT && commands->GetType() != D3D12_COMMAND_LIST_TYPE_COMPUTE)
        || commands->FenceValue != (std::numeric_limits<UINT64>::max)())
    { throw std::invalid_argument("ERROR: Acceleration structure build requires a recording DIRECT or COMPUTE command list."); }
    if (inputs.Type != D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL
        && inputs.Type != D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL)
    { throw std::invalid_argument("ERROR: Expected bottom-level or top-level acceleration structure inputs."); }

    const bool update = (inputs.Flags & D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE) != 0;
    auto prebuildInfo = _prebuildInfo;
    auto buildInputs = inputs;
    if (update)
    {
        constexpr UINT updateFlags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE
            | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
        if (!_buffer.D3DResource || !(_buildFlags & D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE)
            || inputs.Type != _type || inputs.NumDescs != _numDescs || inputs.DescsLayout != _layout
            || (UINT(inputs.Flags) & ~updateFlags) != (UINT(_buildFlags) & ~updateFlags))
        { throw std::invalid_argument("ERROR: AS refit requires a prior ALLOW_UPDATE build with matching type, count, layout and build flags."); }
        buildInputs.Flags = _buildFlags | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
    }
    else
    {
        _device->GetDevice()->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuildInfo);
        if (!prebuildInfo.ResultDataMaxSizeInBytes)
        { throw std::invalid_argument("ERROR: DXR prebuild query returned an empty acceleration structure allocation."); }
    }

    const auto scratchSize = update ? prebuildInfo.UpdateScratchDataSizeInBytes : prebuildInfo.ScratchDataSizeInBytes;
    scratch.Reserve(scratchSize);
    const auto updateSource = update ? GetGPUVirtualAddress() : 0;
    if (!_buffer.D3DResource || _buffer.D3DResource->GetDesc().Width < prebuildInfo.ResultDataMaxSizeInBytes)
    {
        if (update)
        {
            commands->KeepResourceAlive(_buffer.D3DResource.Get());
            commands->ResourceBarrier({_buffer.GetUAVBarrier()});
        }
        _buffer = CreateASBuffer(_device, prebuildInfo.ResultDataMaxSizeInBytes, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
    }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC description{};
    description.Inputs = buildInputs;
    description.DestAccelerationStructureData = GetGPUVirtualAddress();
    description.SourceAccelerationStructureData = updateSource;
    commands->KeepResourceAlive(_buffer.D3DResource.Get());
    if (scratchSize)
    {
        description.ScratchAccelerationStructureData = scratch._buffer.D3DResource->GetGPUVirtualAddress();
        commands->KeepResourceAlive(scratch._buffer.D3DResource.Get());
        commands->ResourceBarrier({scratch._buffer.GetUAVBarrier(), _buffer.GetUAVBarrier()});
    }
    else { commands->ResourceBarrier({_buffer.GetUAVBarrier()}); }
    commands->BuildRaytracingAccelerationStructure(&description);
    commands->ResourceBarrier({_buffer.GetUAVBarrier()});

    if (!update)
    {
        _prebuildInfo = prebuildInfo;
        _type = inputs.Type;
        _buildFlags = inputs.Flags;
        _layout = inputs.DescsLayout;
        _numDescs = inputs.NumDescs;
    }
}

void GDX12AccelerationStructure::Compact(GDX12CommandList* commands, UINT64 compactedSizeInBytes)
{
    if (!commands || commands->GetDevice() != _device
        || (commands->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT && commands->GetType() != D3D12_COMMAND_LIST_TYPE_COMPUTE)
        || commands->FenceValue != (std::numeric_limits<UINT64>::max)())
    { throw std::invalid_argument("ERROR: AS compaction requires a recording DIRECT or COMPUTE command list on the structure's GPU."); }
    if (!_buffer.D3DResource || !(_buildFlags & D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION))
    { throw std::logic_error("ERROR: AS compaction requires a prior ALLOW_COMPACTION build."); }
    if (!compactedSizeInBytes || compactedSizeInBytes > _buffer.D3DResource->GetDesc().Width)
    { throw std::invalid_argument("ERROR: Invalid GPU-reported compacted acceleration structure size."); }
    if (AlignASSize(compactedSizeInBytes) >= _buffer.D3DResource->GetDesc().Width) { return; }

    auto compacted = CreateASBuffer(_device, compactedSizeInBytes, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
    commands->KeepResourceAlive(_buffer.D3DResource.Get());
    commands->KeepResourceAlive(compacted.D3DResource.Get());
    commands->ResourceBarrier({_buffer.GetUAVBarrier()});
    commands->CopyRaytracingAccelerationStructure(compacted.D3DResource->GetGPUVirtualAddress(),
        GetGPUVirtualAddress(), D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT);
    commands->ResourceBarrier({compacted.GetUAVBarrier()});
    _buffer = std::move(compacted);
}
