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
    
    // Returns index of the first non-taken slot in the heap
    UINT GetAvailableIndex();
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

    // Contains indices of recently freed slots
    // GetAvalibleIndex() will first take indices from here
    std::vector<UINT> _avaliableIndices;
};