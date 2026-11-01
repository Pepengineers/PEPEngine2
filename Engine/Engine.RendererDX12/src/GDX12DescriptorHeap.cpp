#include "Engine.RendererDX12/GDX12DescriptorHeap.h"
#include "Engine.RendererDX12/GDX12Device.h"
#include <stdexcept>
#include <algorithm>

GDX12DescriptorHeap::GDX12DescriptorHeap(GDX12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, UINT numDescriptors, D3D12_DESCRIPTOR_HEAP_FLAGS flags)
    : _device(device), 
    _type(type), 
    _numDescriptors(numDescriptors), 
    _heapHeadIndex(0), 
    _flags(flags)
{
    D3D12_DESCRIPTOR_HEAP_DESC desc = {};
    desc.Type = type;
    desc.NumDescriptors = numDescriptors;
    desc.Flags = flags;
    desc.NodeMask = 0;

    ThrowIfFailed(device->GetDevice()->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&_heap)));

    _descriptorSize = device->GetDevice()->GetDescriptorHandleIncrementSize(type);

    _occupanceRegistry.resize(numDescriptors, false);
}

GDX12DescriptorHeap::~GDX12DescriptorHeap()
{
    Reset();
}

const ComPtr<ID3D12DescriptorHeap>& GDX12DescriptorHeap::GetHeap()
{
    return _heap;
}

D3D12_DESCRIPTOR_HEAP_TYPE GDX12DescriptorHeap::GetType()
{
    return _type;
}

D3D12_DESCRIPTOR_HEAP_FLAGS GDX12DescriptorHeap::GetFlags()
{
    return _flags;
}

UINT GDX12DescriptorHeap::GetNumDescriptors()
{
    return _numDescriptors;
}

UINT GDX12DescriptorHeap::GetAvailableIndex(UINT startIndex, UINT maxRange)
{
    if (startIndex >= _numDescriptors) throw std::out_of_range("Descriptor allocation starts outside heap");
    const UINT remaining = _numDescriptors - startIndex;
    const UINT count = maxRange == 0 ? remaining : (std::min)(maxRange, remaining);
    for (UINT offset = 0; offset < count; ++offset)
    {
        const UINT index = startIndex + offset;
        if (_occupanceRegistry[index]) continue;
        _occupanceRegistry[index] = true;
        return index;
    }
    throw std::runtime_error("Descriptor allocation range exhausted");
}

D3D12_CPU_DESCRIPTOR_HANDLE GDX12DescriptorHeap::GetCPUHandle(UINT index) const
{
    return CD3DX12_CPU_DESCRIPTOR_HANDLE(_heap->GetCPUDescriptorHandleForHeapStart(), index, _descriptorSize);
}

D3D12_GPU_DESCRIPTOR_HANDLE GDX12DescriptorHeap::GetGPUHandle(UINT index) const
{
    return CD3DX12_GPU_DESCRIPTOR_HANDLE(_heap->GetGPUDescriptorHandleForHeapStart(), index, _descriptorSize);
}

void GDX12DescriptorHeap::Reset()
{
    _heap.Reset();
    _numDescriptors = 0;
    _descriptorSize = 0;
    _heapHeadIndex = 0;
}