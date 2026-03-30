#pragma once

#include "Engine.RendererDX12/D3DHelpers.h"

#include "Engine.RendererDX12/GDX12Device.h"

template<typename T>
class GDX12UploadBuffer
{
public:
    inline GDX12UploadBuffer(GDX12Device* device, UINT elementCount, bool useConstantBufferSizeAlignment = true);
	~GDX12UploadBuffer();

	ComPtr<ID3D12Resource> GetResource();

    void CopyData(UINT elementIndex, const T& data);
    void CopyData(UINT elementIndex, const T* data, UINT count);

    //Creates new buffer and copies old data into it(if possible)
    void Resize(UINT newElementCount);
    UINT GetElementCount();
    UINT GetElementSize();

    // 0'th element is the buffer's address
    D3D12_GPU_VIRTUAL_ADDRESS GetElementAddress(UINT elementIndex);

private:
    GDX12Device* _device;
    ComPtr<ID3D12Resource> _uploadBuffer;
    BYTE* _mappedData = nullptr;

    UINT _elementCount;
    UINT _elementByteSize;
    UINT _totalBufferSize;
    bool _useConstantBufferSizeAlignment;
};

//Can't move template class definitions into .cpp cuz it'll throw linking errors

template<typename T>
inline GDX12UploadBuffer<T>::GDX12UploadBuffer(GDX12Device* device, UINT elementCount, bool useConstantBufferSizeAlignment)
    : _device(device)
    , _elementCount(elementCount)
    , _useConstantBufferSizeAlignment(useConstantBufferSizeAlignment)
{
    _elementByteSize = sizeof(T);

    // constant buffer size should be a multiple of 255
    if (_useConstantBufferSizeAlignment) { _elementByteSize = (_elementByteSize + 255) & ~255; }

    _totalBufferSize = _elementByteSize * _elementCount;

    D3D12_RESOURCE_DESC resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(_totalBufferSize);
    CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_UPLOAD);

    ThrowIfFailed(device->GetDevice()->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &resourceDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&_uploadBuffer)));

    //keep the resource mapped for quick access
    ThrowIfFailed(_uploadBuffer->Map(0, nullptr, reinterpret_cast<void**>(&_mappedData)));
}

template<typename T>
GDX12UploadBuffer<T>::~GDX12UploadBuffer()
{
    if (_uploadBuffer && _mappedData)
    {
        _uploadBuffer->Unmap(0, nullptr);
        _mappedData = nullptr;
    }
}

template<typename T>
ComPtr<ID3D12Resource> GDX12UploadBuffer<T>::GetResource()
{
    return _uploadBuffer.Get();
}

template<typename T>
void GDX12UploadBuffer<T>::CopyData(UINT elementIndex, const T& data)
{
    if (elementIndex >= _elementCount) return;
    memcpy(&_mappedData[elementIndex * _elementByteSize], &data, sizeof(T));
}

template<typename T>
void GDX12UploadBuffer<T>::CopyData(UINT elementIndex, const T* data, UINT count)
{
    if (elementIndex + count > _elementCount) return;
    memcpy(&_mappedData[elementIndex * _elementByteSize], data, sizeof(T) * count);
}

template<typename T>
void GDX12UploadBuffer<T>::Resize(UINT newElementCount)
{
    if (newElementCount <= _elementCount) return;

    UINT newTotalSize = _elementByteSize * newElementCount;

    D3D12_RESOURCE_DESC resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(newTotalSize);
    CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_UPLOAD);

    ComPtr<ID3D12Resource> newBuffer;
    ThrowIfFailed(_device->GetDevice()->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &resourceDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&newBuffer)));

    BYTE* newMappedData = nullptr;
    ThrowIfFailed(newBuffer->Map(0, nullptr, reinterpret_cast<void**>(&newMappedData)));

    if (_uploadBuffer && _mappedData)
    {
        memcpy(newMappedData, _mappedData, _elementByteSize * _elementCount);
        _uploadBuffer->Unmap(0, nullptr);
    }

    _uploadBuffer = std::move(newBuffer);
    _mappedData = newMappedData;
    _elementCount = newElementCount;
    _totalBufferSize = newTotalSize;
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
    return _uploadBuffer->GetGPUVirtualAddress() + elementIndex * _elementByteSize;
}