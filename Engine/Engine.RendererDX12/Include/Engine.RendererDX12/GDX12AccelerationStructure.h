#pragma once

#include "Engine.RendererDX12/GDX12Resource.h"

class GDX12Device;
class GDX12CommandList;

// Temporary build storage shared by sequential BLAS and TLAS builds on one GPU.
// Cross-queue reuse requires the caller to order those queues with fences.
class GDX12RayTracingScratchBuffer
{
public:
    explicit GDX12RayTracingScratchBuffer(GDX12Device* device);
    GDX12RayTracingScratchBuffer(const GDX12RayTracingScratchBuffer&) = delete;
    GDX12RayTracingScratchBuffer& operator=(const GDX12RayTracingScratchBuffer&) = delete;

    // Grow only when needed, recorded commands retain the previous allocation.
    void Reserve(UINT64 sizeInBytes);
    GDX12Resource& GetResource() { return _buffer; }

private:
    friend class GDX12AccelerationStructure;
    GDX12Device* _device;
    GDX12Resource _buffer;
};

// Owns one device-local BLAS or TLAS, its state stays RAYTRACING_ACCELERATION_STRUCTURE.
// Scene ownership, geometry uploads and TLAS instance data belong to the caller.
class GDX12AccelerationStructure
{
public:
    explicit GDX12AccelerationStructure(GDX12Device* device);
    GDX12AccelerationStructure(const GDX12AccelerationStructure&) = delete;
    GDX12AccelerationStructure& operator=(const GDX12AccelerationStructure&) = delete;

    // Record a full build or a refit when Inputs.Flags includes PERFORM_UPDATE.
    // Refit requires a prior ALLOW_UPDATE build with compatible geometry and instance layout.
    // Reuses the allocation when it fits, a refit after compaction can grow storage while retaining its source.
    // Input buffers must be readable by non-pixel shaders and remain alive until GPU completion.
    // The caller retains referenced BLAS for TLAS traversal, and orders uses on different queues.
    // Scratch reuse and AS reads/writes are ordered with UAV barriers, no CPU wait is performed.
    void Build(GDX12CommandList* commands, const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS& inputs,
        GDX12RayTracingScratchBuffer& scratch);

    // Record compaction into a smaller allocation using the GPU-reported compacted size.
    // Requires ALLOW_COMPACTION on the original build, source and destination live until completion.
    void Compact(GDX12CommandList* commands, UINT64 compactedSizeInBytes);

    // Before recording a later use, retain this resource through commands->KeepResourceAlive().
    // Building or compaction may change this address, dependent TLAS instance data must then be updated.
    D3D12_GPU_VIRTUAL_ADDRESS GetGPUVirtualAddress() const;
    GDX12Resource& GetResource() { return _buffer; }
    const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO& GetPrebuildInfo() const { return _prebuildInfo; }

private:
    GDX12Device* _device;
    GDX12Resource _buffer;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO _prebuildInfo{};
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE _type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS _buildFlags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE;
    D3D12_ELEMENTS_LAYOUT _layout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    UINT _numDescs = 0;
};
