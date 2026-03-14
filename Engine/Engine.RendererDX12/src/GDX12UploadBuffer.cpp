#include "Engine.RendererDX12/GDX12UploadBuffer.h"

#include "Engine.RendererDX12/GDX12Device.h"

template<typename T>
GDX12UploadBuffer<T>::GDX12UploadBuffer(std::shared_ptr<GDX12Device> device, UINT elementCount, bool isConstantBuffer)
    : _device(device)
    , _elementCount(elementCount)
    , _isConstantBuffer(isConstantBuffer)
{
    _elementByteSize = sizeof(T);

    // constant buffer size should be a multiple of 255
    if (_isConstantBuffer) { _elementByteSize = (_elementByteSize + 255) & ~255; }

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
