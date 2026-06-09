#pragma once

#include "Engine.RendererDX12/D3DHelpers.h"

#include "Engine.RendererDX12/GDX12Device.h"
#include "Engine.RendererDX12/GDX12Descriptor.h"
#include "Engine.RendererDX12/GDX12DescriptorHeap.h"

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

    ComPtr<ID3D12Resource> GetResource();

    void CopyData(UINT elementIndex, const T& data);
    void CopyData(UINT elementIndex, const T* data, UINT count);

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
    ComPtr<ID3D12Resource> _buffer;
    BYTE* _mappedData = nullptr;

    UINT _elementCount;
    UINT _elementByteSize;
    UINT _totalBufferSize;
    EBufferType _bufferType;
    bool _useConstantBufferSizeAlignment;

    std::unique_ptr<GDX12Descriptor> _srv;
    std::unique_ptr<GDX12Descriptor> _uav;
    GDX12DescriptorHeap* _heap;
    UINT _heapIndex;
};

template<typename T>
inline GDX12UploadBuffer<T>::GDX12UploadBuffer(GDX12Device* device, UINT elementCount, EBufferType bufferType, bool useConstantBufferSizeAlignment)
    : _device(device), _elementCount(elementCount), _bufferType(bufferType), _useConstantBufferSizeAlignment(useConstantBufferSizeAlignment), _heap(nullptr), _heapIndex(0)
{
    _elementByteSize = sizeof(T);
    if (_useConstantBufferSizeAlignment && _bufferType == EBufferType::Upload)
    {
        _elementByteSize = (_elementByteSize + 255) & ~255;
    }

    if (elementCount == 0)
    {
        _buffer = nullptr;
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
        CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
        ThrowIfFailed(_device->GetDevice()->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &resourceDesc,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&_buffer)));
    }
    else
    {
        CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_UPLOAD);
        ThrowIfFailed(_device->GetDevice()->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &resourceDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&_buffer)));
        ThrowIfFailed(_buffer->Map(0, nullptr, reinterpret_cast<void**>(&_mappedData)));
    }
}

template<typename T>
GDX12UploadBuffer<T>::~GDX12UploadBuffer()
{
    if (_buffer && _mappedData)
    {
        _buffer->Unmap(0, nullptr);
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
        _heapIndex = HeapIndex;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = DXGI_FORMAT_UNKNOWN;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srvDesc.Buffer.FirstElement = 0;
    srvDesc.Buffer.NumElements = _elementCount;
    srvDesc.Buffer.StructureByteStride = sizeof(T);

    _srv->InitAsSRV(_buffer.Get(), &srvDesc, inHeap, HeapIndex);
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
    }

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.FirstElement = 0;
    uavDesc.Buffer.NumElements = _elementCount;
    uavDesc.Buffer.StructureByteStride = sizeof(T);
    uavDesc.Buffer.CounterOffsetInBytes = 0;
    uavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
    uavDesc.Format = DXGI_FORMAT_UNKNOWN;

    _uav->InitAsUAV(_buffer.Get(), nullptr, &uavDesc, inHeap, HeapIndex);
}

template<typename T>
GDX12Descriptor* GDX12UploadBuffer<T>::GetUAV()
{
    return _uav.get();
}

template<typename T>
ComPtr<ID3D12Resource> GDX12UploadBuffer<T>::GetResource()
{
    return _buffer;
}

template<typename T>
void GDX12UploadBuffer<T>::CopyData(UINT elementIndex, const T& data)
{
    if (!_buffer) return;
    if (elementIndex >= _elementCount) return;

    if (_bufferType == EBufferType::Upload && _mappedData)
    {
        memcpy(&_mappedData[elementIndex * _elementByteSize], &data, sizeof(T));
    }
}

template<typename T>
void GDX12UploadBuffer<T>::CopyData(UINT elementIndex, const T* data, UINT count)
{
    if (!_buffer) return;
    if (elementIndex + count > _elementCount) return;

    if (_bufferType == EBufferType::Upload && _mappedData)
    {
        memcpy(&_mappedData[elementIndex * _elementByteSize], data, sizeof(T) * count);
    }
}

template<typename T>
void GDX12UploadBuffer<T>::Resize(UINT newElementCount)
{
    if (newElementCount <= _elementCount) return;

    UINT oldElementCount = _elementCount;
    _elementCount = newElementCount;

    if (_bufferType == EBufferType::Upload && _mappedData)
    {
        _buffer->Unmap(0, nullptr);
        _mappedData = nullptr;
    }

    CreateBuffer();

    if (_srv) CreateSRV(_heap, _heapIndex);
    if (_uav) CreateUAV(_heap, _heapIndex);
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
    if (!_buffer) return 0;
    return _buffer->GetGPUVirtualAddress() + elementIndex * _elementByteSize;
}