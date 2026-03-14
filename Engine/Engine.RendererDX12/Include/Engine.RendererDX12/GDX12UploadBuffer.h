#pragma once

#include "Engine.RendererDX12/D3DHelpers.h"

class GDX12Device;

template<typename T>
class GDX12UploadBuffer
{
public:
	GDX12UploadBuffer(std::shared_ptr<GDX12Device> device, UINT elementCount, bool isConstantBuffer = true);
	~GDX12UploadBuffer();

	ComPtr<ID3D12Resource> GetResource();

    void CopyData(UINT elementIndex, const T& data);
    void CopyData(UINT elementIndex, const T* data, UINT count);

    //Creates new buffer and copies old data into it(if possible)
    void Resize(UINT newElementCount);
    UINT GetElementCount();
    UINT GetElementSize();

private:
    std::shared_ptr<GDX12Device> _device;
    ComPtr<ID3D12Resource> _uploadBuffer;
    BYTE* _mappedData = nullptr;

    UINT _elementCount;
    UINT _elementByteSize;
    UINT _totalBufferSize;
    bool _isConstantBuffer;
};