#pragma once

#include "Engine.RendererDX12/D3DHelpers.h"
#include "Engine.RendererDX12/GDX12Device.h"
#include "Engine.RendererDX12/GDX12Descriptor.h"
#include "Engine.RendererDX12/GDX12DescriptorHeap.h"
#include "Engine.RendererDX12/GDX12CommandList.h"

template<typename T>
class GDX12AppendBuffer
{
public:
    GDX12AppendBuffer(GDX12Device* device, UINT maxElementCount);
    ~GDX12AppendBuffer();

    void CreateUAVs(GDX12DescriptorHeap* shaderVisibleHeap, UINT heapIndex);

    ID3D12Resource* GetResource() const { return _buffer.Get(); }
    ID3D12Resource* GetCounterBuffer() const { return _counterBuffer.Get(); }
    UINT GetMaxElementCount() const { return _maxElementCount; }
    UINT GetElementSize() const { return _elementSize; }

    GDX12Descriptor* GetUAV() const { return _uav.get(); }
    GDX12Descriptor* GetCounterUAV() const { return _counterUAV.get(); }

private:
    void CreateBuffers();

    GDX12Device* _device;
    ComPtr<ID3D12Resource> _buffer;
    ComPtr<ID3D12Resource> _counterBuffer;
    ComPtr<ID3D12Resource> _readbackBuffer;

    UINT _maxElementCount;
    UINT _elementSize;
    UINT _totalBufferSize;

    std::unique_ptr<GDX12Descriptor> _uav;         // Main buffer UAV (structured, with counter)
    std::unique_ptr<GDX12Descriptor> _counterUAV;  // Counter buffer UAV (raw, for reading/writing)
};

template<typename T>
GDX12AppendBuffer<T>::GDX12AppendBuffer(GDX12Device* device, UINT maxElementCount)
    : _device(device), _maxElementCount(maxElementCount)
{
    _elementSize = sizeof(T);
    _totalBufferSize = _elementSize * _maxElementCount;
    CreateBuffers();
}

template<typename T>
GDX12AppendBuffer<T>::~GDX12AppendBuffer() = default;

template<typename T>
void GDX12AppendBuffer<T>::CreateBuffers()
{
    D3D12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(_totalBufferSize);
    bufferDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);
    ThrowIfFailed(_device->GetDevice()->CreateCommittedResource(
        &defaultHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&_buffer)));

    auto counterDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(UINT),
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    ThrowIfFailed(_device->GetDevice()->CreateCommittedResource(
        &defaultHeap, D3D12_HEAP_FLAG_NONE, &counterDesc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&_counterBuffer)));
}

template<typename T>
void GDX12AppendBuffer<T>::CreateUAVs(GDX12DescriptorHeap* shaderVisibleHeap, UINT heapIndex)
{
    // Main buffer UAV (structured, with counter for Append)
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.FirstElement = 0;
    uavDesc.Buffer.NumElements = _maxElementCount;
    uavDesc.Buffer.StructureByteStride = _elementSize;
    uavDesc.Buffer.CounterOffsetInBytes = 0;
    uavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
    uavDesc.Format = DXGI_FORMAT_UNKNOWN;

    _uav = std::make_unique<GDX12Descriptor>();
    _uav->InitAsUAV(_buffer.Get(), _counterBuffer.Get(), &uavDesc,
        shaderVisibleHeap, heapIndex);

    // Counter buffer UAV (raw, for reading/writing from compute shader)
    D3D12_UNORDERED_ACCESS_VIEW_DESC counterUAVDesc = {};
    counterUAVDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    counterUAVDesc.Buffer.FirstElement = 0;
    counterUAVDesc.Buffer.NumElements = 1;
    counterUAVDesc.Buffer.StructureByteStride = 0;
    counterUAVDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
    counterUAVDesc.Format = DXGI_FORMAT_R32_UINT;

    _counterUAV = std::make_unique<GDX12Descriptor>();
    _counterUAV->InitAsUAV(_counterBuffer.Get(), nullptr, &counterUAVDesc,
        shaderVisibleHeap, heapIndex + 1);
}