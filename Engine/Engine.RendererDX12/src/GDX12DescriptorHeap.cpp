#include "Engine.RendererDX12/GDX12DescriptorHeap.h"
#include "Engine.RendererDX12/GDX12Device.h"
#include <algorithm>
#include <stdexcept>

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
    if (startIndex >= _numDescriptors || maxRange > _numDescriptors - startIndex)
    { throw std::invalid_argument("ERROR: Requested descriptor range is outside the heap."); }
    const auto begin = _occupanceRegistry.begin() + startIndex;
    const auto end = maxRange ? begin + maxRange : _occupanceRegistry.end();
    const auto first = std::find(begin, end, false);
    if (first == end)
    { throw std::runtime_error("ERROR: No free descriptors in the requested heap range."); }
    *first = true;
    return static_cast<UINT>(first - _occupanceRegistry.begin());
}

UINT GDX12DescriptorHeap::AllocateRange(UINT count, UINT startIndex)
{
    if (count == 0 || startIndex > _numDescriptors || count > _numDescriptors - startIndex)
    { throw std::invalid_argument("ERROR: Invalid descriptor heap range."); }
    const auto first = std::search_n(_occupanceRegistry.begin() + startIndex, _occupanceRegistry.end(), count, false);
    if (first == _occupanceRegistry.end())
    { throw std::runtime_error("ERROR: Descriptor heap has no contiguous range of the requested size."); }
    std::fill_n(first, count, true);
    return static_cast<UINT>(first - _occupanceRegistry.begin());
}

void GDX12DescriptorHeap::ReleaseRange(UINT firstIndex, UINT count)
{
    if (firstIndex > _numDescriptors || count > _numDescriptors - firstIndex)
    { throw std::out_of_range("ERROR: Released descriptor range is outside the heap."); }
    std::fill_n(_occupanceRegistry.begin() + firstIndex, count, false);
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
