#pragma once

#include "Engine.RendererDX12/D3DHelpers.h"

class GDX12Device;

class GDX12DescriptorHeap
{
    friend class GDX12Descriptor;
    friend class GDX12Texture;

public:
    GDX12DescriptorHeap(GDX12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, UINT numDescriptors, D3D12_DESCRIPTOR_HEAP_FLAGS flags);
    ~GDX12DescriptorHeap();

    const ComPtr<ID3D12DescriptorHeap>& GetHeap();

    // Get heap properties
    D3D12_DESCRIPTOR_HEAP_TYPE GetType();
    D3D12_DESCRIPTOR_HEAP_FLAGS GetFlags();
    UINT GetNumDescriptors();
    
    // Returns the first free slot in [startIndex, startIndex + maxRange).
    // maxRange == 0 searches to the end of the heap. Throws if no slot is available
    // or the requested range is outside the heap.
    UINT GetAvailableIndex(UINT startIndex = 0, UINT maxRange = 0);

    // Reserves contiguous slots for APIs that write a descriptor range themselves.
    // The caller owns the range and must release it before this heap is destroyed.
    UINT AllocateRange(UINT count, UINT startIndex = 0);
    // Releases a previously reserved range after its GPU users have completed.
    // Throws if the range is outside the heap.
    void ReleaseRange(UINT firstIndex, UINT count);

    D3D12_CPU_DESCRIPTOR_HANDLE GetCPUHandle(UINT index = 0) const;
    D3D12_GPU_DESCRIPTOR_HANDLE GetGPUHandle(UINT index = 0) const;

    void Reset();

private:
    GDX12Device* _device;
    ComPtr<ID3D12DescriptorHeap> _heap;
    D3D12_DESCRIPTOR_HEAP_FLAGS _flags;

    D3D12_DESCRIPTOR_HEAP_TYPE _type;
    UINT _numDescriptors;
    UINT _descriptorSize;

    // Index of the first non-taken element in the heap
    UINT _heapHeadIndex;

    // Contains bool IsSlotOccupied for each slot in heap
    std::vector<bool> _occupanceRegistry;
};

// Don't forget to update these values in CBufferStructures.hlsli
enum GDX12SRVHeapIndexAllocation
{
    GBuffer_Color = 0,
    GBuffer_Normal = 1,
    GBuffer_Depth = 2,
    GBuffer_RangeLength = 3,

    TLAS = 10,

    ShadowMaps_StartIndex = 500,
    ShadowMaps_RangeLength = 999,

    Texture2D_StartIndex = 1000,
    Texture2D_RangeLength = 99000,

    TextureCube_StartIndex = Texture2D_StartIndex + Texture2D_RangeLength,
    TextureCube_RangeLength = 5000,

    // Frame data, FrameGraph resources and pass/SDK descriptors use the remaining
    // heap after the bindless texture tables
    RendererResources_StartIndex = TextureCube_StartIndex + TextureCube_RangeLength,
};

// Might need to add DSV and RTV heap allocators later
