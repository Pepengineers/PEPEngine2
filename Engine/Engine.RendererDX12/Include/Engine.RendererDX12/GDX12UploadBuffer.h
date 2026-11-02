#pragma once

#include "Engine.RendererDX12/D3DHelpers.h"

#include "Engine.RendererDX12/GDX12Device.h"
#include "Engine.RendererDX12/GDX12Descriptor.h"
#include "Engine.RendererDX12/GDX12Resource.h"
#include "Engine.RendererDX12/GDX12DescriptorHeap.h"
#include <cstring>
#include <utility>
#include <atomic>

enum class EBufferType
{
    Upload,
    Default
};

template<typename T>
class GDX12UploadBuffer
{
public:
    inline GDX12UploadBuffer(GDX12Device* device, UINT elementCount, EBufferType bufferType = EBufferType::Upload, bool useConstantBufferSizeAlignment = true);
    ~GDX12UploadBuffer();

    GDX12Resource& GetResource();

    void CopyData(UINT elementIndex, const T& data);
    void CopyData(UINT elementIndex, const T* data, UINT count);

    // CPU mirror avoids reading write-combined upload memory when checking static templates.
    void TrackChanges() { _cpuData.resize(_elementCount); _trackChanges = true; }
    uint64_t GetRevision() const { return _revision.load(std::memory_order_relaxed); }
    const T* GetCPUData() const { return _cpuData.data(); }

    void Resize(UINT newElementCount);
    UINT GetElementCount();
    UINT GetElementSize();

    D3D12_GPU_VIRTUAL_ADDRESS GetElementAddress(UINT elementIndex);

    GDX12Descriptor* GetSRV();
    void CreateSRV(GDX12DescriptorHeap* inHeap, UINT HeapIndex);

    GDX12Descriptor* GetUAV();
    void CreateUAV(GDX12DescriptorHeap* inHeap, UINT HeapIndex);

private:
    void CreateBuffer();

    GDX12Device* _device;
    GDX12Resource _buffer;
    BYTE* _mappedData = nullptr;

    UINT _elementCount;
    UINT _elementByteSize;
    UINT _totalBufferSize;
    EBufferType _bufferType;
    bool _useConstantBufferSizeAlignment;

    std::unique_ptr<GDX12Descriptor> _srv;
    std::unique_ptr<GDX12Descriptor> _uav;
    GDX12DescriptorHeap* _heap;
    UINT _SRVheapIndex;
    UINT _UAVheapIndex;
    bool _trackChanges = false;
    std::vector<T> _cpuData;
    std::atomic<uint64_t> _revision{1};
};

template<typename T>
inline GDX12UploadBuffer<T>::GDX12UploadBuffer(GDX12Device* device, UINT elementCount, EBufferType bufferType, bool useConstantBufferSizeAlignment)
    : _device(device), _elementCount(elementCount), _bufferType(bufferType), 
    _useConstantBufferSizeAlignment(useConstantBufferSizeAlignment), _heap(nullptr), 
    _SRVheapIndex(0), _UAVheapIndex(0)
{
    _elementByteSize = sizeof(T);
    if (_useConstantBufferSizeAlignment && _bufferType == EBufferType::Upload)
    {
        _elementByteSize = (_elementByteSize + 255) & ~255;
    }

    if (elementCount == 0)
    {
        _mappedData = nullptr;
        _totalBufferSize = 0;
        return;
    }

    CreateBuffer();
}

template<typename T>
void GDX12UploadBuffer<T>::CreateBuffer()
{
    _totalBufferSize = _elementByteSize * _elementCount;
    D3D12_RESOURCE_DESC resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(_totalBufferSize);

    if (_bufferType == EBufferType::Default)
    {
        resourceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        _buffer.D3DResource = _device->CreateResource(D3D12_HEAP_TYPE_DEFAULT, resourceDesc,
            D3D12_RESOURCE_STATE_COMMON, nullptr);
    }
    else
    {
        _buffer.D3DResource = _device->CreateResource(D3D12_HEAP_TYPE_UPLOAD, resourceDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr);
        ThrowIfFailed(_buffer.D3DResource->Map(0, nullptr, reinterpret_cast<void**>(&_mappedData)));
        // Reused heap regions need the same initial contents as fresh committed upload buffers.
        std::memset(_mappedData, 0, _totalBufferSize);
    }
}

template<typename T>
GDX12UploadBuffer<T>::~GDX12UploadBuffer()
{
    if (_buffer.D3DResource && _mappedData)
    {
        _buffer.D3DResource->Unmap(0, nullptr);
        _mappedData = nullptr;
    }
}

template<typename T>
void GDX12UploadBuffer<T>::CreateSRV(GDX12DescriptorHeap* inHeap, UINT HeapIndex)
{
    if (!_srv)
    {
        _srv = std::make_unique<GDX12Descriptor>();
        _heap = inHeap;
        _SRVheapIndex = HeapIndex;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = DXGI_FORMAT_UNKNOWN;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srvDesc.Buffer.FirstElement = 0;
    srvDesc.Buffer.NumElements = _elementCount;
    srvDesc.Buffer.StructureByteStride = sizeof(T);

    _srv->InitAsSRV(_buffer.D3DResource.Get(), &srvDesc, inHeap, HeapIndex);
}

template<typename T>
GDX12Descriptor* GDX12UploadBuffer<T>::GetSRV()
{
    return _srv.get();
}

template<typename T>
void GDX12UploadBuffer<T>::CreateUAV(GDX12DescriptorHeap* inHeap, UINT HeapIndex)
{
    if (!_uav)
    {
        _uav = std::make_unique<GDX12Descriptor>();
        _heap = inHeap;
        _UAVheapIndex = HeapIndex;
    }

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.FirstElement = 0;
    uavDesc.Buffer.NumElements = _elementCount;
    uavDesc.Buffer.StructureByteStride = sizeof(T);
    uavDesc.Buffer.CounterOffsetInBytes = 0;
    uavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
    uavDesc.Format = DXGI_FORMAT_UNKNOWN;

    _uav->InitAsUAV(_buffer.D3DResource.Get(), nullptr, &uavDesc, inHeap, HeapIndex);
}

template<typename T>
GDX12Descriptor* GDX12UploadBuffer<T>::GetUAV()
{
    return _uav.get();
}

template<typename T>
GDX12Resource& GDX12UploadBuffer<T>::GetResource()
{
    return _buffer;
}

template<typename T>
void GDX12UploadBuffer<T>::CopyData(UINT elementIndex, const T& data)
{
    if (!_buffer.D3DResource) return;
    if (elementIndex >= _elementCount) return;
    CopyData(elementIndex, &data, 1);
}

template<typename T>
void GDX12UploadBuffer<T>::CopyData(UINT elementIndex, const T* data, UINT count)
{
    if (!_buffer.D3DResource) return;
    if (elementIndex + count > _elementCount) return;

    if (_bufferType == EBufferType::Upload && _mappedData)
    {
        if (_trackChanges)
        {
            if (std::memcmp(&_cpuData[elementIndex], data, sizeof(T) * count) == 0) return;
            std::memcpy(&_cpuData[elementIndex], data, sizeof(T) * count);
            _revision.fetch_add(1, std::memory_order_relaxed);
        }
        memcpy(&_mappedData[elementIndex * _elementByteSize], data, sizeof(T) * count);
    }
}

template<typename T>
void GDX12UploadBuffer<T>::Resize(UINT newElementCount)
{
    if (newElementCount <= _elementCount) return;

    GDX12UploadBuffer replacement(_device, newElementCount, _bufferType, _useConstantBufferSizeAlignment);
    if (_mappedData) std::memcpy(replacement._mappedData, _mappedData, _totalBufferSize);
    std::swap(_buffer, replacement._buffer);
    std::swap(_mappedData, replacement._mappedData);
    std::swap(_elementCount, replacement._elementCount);
    std::swap(_totalBufferSize, replacement._totalBufferSize);
    if (_trackChanges)
    {
        _cpuData.resize(newElementCount);
        _revision.fetch_add(1, std::memory_order_relaxed);
    }

    if (_srv) CreateSRV(_heap, _SRVheapIndex);
    if (_uav) CreateUAV(_heap, _UAVheapIndex);
}

template<typename T>
UINT GDX12UploadBuffer<T>::GetElementCount()
{
    return _elementCount;
}

template<typename T>
UINT GDX12UploadBuffer<T>::GetElementSize()
{
    return _elementByteSize;
}

template<typename T>
D3D12_GPU_VIRTUAL_ADDRESS GDX12UploadBuffer<T>::GetElementAddress(UINT elementIndex)
{
    if (!_buffer.D3DResource) return 0;
    return _buffer.D3DResource->GetGPUVirtualAddress() + elementIndex * _elementByteSize;
}
