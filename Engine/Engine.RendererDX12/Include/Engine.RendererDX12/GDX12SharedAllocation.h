#pragma once

#include "Engine.RendererDX12/D3DHelpers.h"
#include "Engine.RendererDX12/GDX12Device.h"
#include "Engine.RendererDX12/GDX12TextureResource.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

class GDX12SharedAllocation
{
public:
    GDX12SharedAllocation() = default;
    ~GDX12SharedAllocation() { Release(); }

    GDX12SharedAllocation(const GDX12SharedAllocation&) = delete;
    GDX12SharedAllocation& operator=(const GDX12SharedAllocation&) = delete;


    HRESULT Initialize(GDX12Device* source, GDX12Device* target, UINT width, UINT height, DXGI_FORMAT format)
    {
        const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(format, width, height, 1, 1, 1, 0,
            D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER, D3D12_TEXTURE_LAYOUT_ROW_MAJOR);
        return Initialize(source, target, desc);
    }

    HRESULT Initialize(GDX12Device* source, GDX12Device* target, const D3D12_RESOURCE_DESC& description)
    {
        Release();
        if (!source || !target || !source->IsInitialized() || !target->IsInitialized())
        {
            throw std::invalid_argument("ERROR: Shared allocation requires initialized source and target devices. Initialize the supplied source and "
                "target GDX12Device objects before creating a shared allocation.");
        }
        auto desc = description;
        desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
        if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
        {
            if (!desc.Width || desc.Height != 1 || desc.DepthOrArraySize != 1 || desc.MipLevels != 1 ||
                desc.Format != DXGI_FORMAT_UNKNOWN || desc.SampleDesc.Count != 1 ||
                desc.Layout != D3D12_TEXTURE_LAYOUT_ROW_MAJOR ||
                (desc.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)))
            {
                throw std::invalid_argument("ERROR: Given cross-adapter buffer description is invalid. Use a non-empty ROW_MAJOR buffer with "
                    "DXGI_FORMAT_UNKNOWN, one mip, one slice, no MSAA, and no render-target or depth-stencil flags.");
            }
        }
        else if (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D)
        {
            if (!desc.Width || !desc.Height || desc.Width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
                desc.Height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION || desc.DepthOrArraySize != 1 ||
                desc.MipLevels != 1 || desc.SampleDesc.Count != 1 || desc.SampleDesc.Quality ||
                desc.Format == DXGI_FORMAT_UNKNOWN || desc.Layout != D3D12_TEXTURE_LAYOUT_ROW_MAJOR ||
                (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))
            {
                throw std::invalid_argument("ERROR: Given ROW_MAJOR shared texture description is unsupported. Use supported non-zero dimensions, a typed "
                    "format, one slice, one mip, no MSAA, and no depth attachment.");
            }
        }
        else { throw std::invalid_argument("ERROR: Given shared allocation has an unsupported resource dimension. Use a BUFFER or TEXTURE2D resource "
            "description."); }

        const auto sourceInfo = source->GetDevice()->GetResourceAllocationInfo(0, 1, &desc);
        const auto targetInfo = target->GetDevice()->GetResourceAllocationInfo(0, 1, &desc);
        auto validInfo = [](const D3D12_RESOURCE_ALLOCATION_INFO& info)
        {
            return info.SizeInBytes && info.SizeInBytes != (std::numeric_limits<UINT64>::max)() &&
                info.Alignment && !(info.Alignment & (info.Alignment - 1));
        };
        if (!validInfo(sourceInfo) || !validInfo(targetInfo))
        {
            return DXGI_ERROR_UNSUPPORTED;
        }
        const UINT64 alignment = (std::max)(sourceInfo.Alignment, targetInfo.Alignment);
        const UINT64 size = (std::max)(sourceInfo.SizeInBytes, targetInfo.SizeInBytes);
        if (size > (std::numeric_limits<UINT64>::max)() - (alignment - 1))
        {
            throw std::overflow_error("ERROR: Cross-adapter heap size alignment overflow. Reduce the shared allocation size so its aligned byte "
                "count fits in UINT64.");
        }

        ComPtr<ID3D12Heap> sourceHeap;
        ComPtr<ID3D12Heap> targetHeap;
        ComPtr<ID3D12Resource> sourceResource;
        ComPtr<ID3D12Resource> targetResource;
        D3D12_HEAP_DESC heapDesc = {};
        heapDesc.SizeInBytes = (size + alignment - 1) & ~(alignment - 1);
        heapDesc.Alignment = alignment;
        heapDesc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        heapDesc.Properties.CreationNodeMask = 1;
        heapDesc.Properties.VisibleNodeMask = 1;
        heapDesc.Flags = D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER;
        HRESULT result = source->GetDevice()->CreateHeap(&heapDesc, IID_PPV_ARGS(&sourceHeap));
        if (FAILED(result)) { return result; }
        result = source->GetDevice()->CreatePlacedResource(sourceHeap.Get(), 0, &desc,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&sourceResource));
        if (FAILED(result)) { return result; }
        if (source == target)
        {
            targetHeap = sourceHeap;
            targetResource = sourceResource;
        }
        else
        {
            HANDLE handle = nullptr;
            result = source->GetDevice()->CreateSharedHandle(sourceHeap.Get(), nullptr, GENERIC_ALL, nullptr, &handle);
            if (FAILED(result)) { return result; }
            if (!handle) { return E_HANDLE; }
            const HRESULT opened = target->GetDevice()->OpenSharedHandle(handle, IID_PPV_ARGS(&targetHeap));
            CloseHandle(handle);
            if (FAILED(opened)) { return opened; }
            result = target->GetDevice()->CreatePlacedResource(targetHeap.Get(), 0, &desc,
                D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&targetResource));
            if (FAILED(result)) { return result; }
        }
        _sourceDevice = source;
        _targetDevice = target;
        _description = desc;
        _heapSize = heapDesc.SizeInBytes;
        _sharedHeap = sourceHeap;
        _openedHeap = targetHeap;
        _sharedResourceSource.D3DResource = sourceResource;
        _sharedResourceTarget.D3DResource = targetResource;
        _isInitialized = true;
        return S_OK;
    }

    void Release()
    {
        _isInitialized = false;
        _sharedResourceSource.Reset();
        _sharedResourceTarget.Reset();
        _openedHeap.Reset();
        _sharedHeap.Reset();
        _sourceDevice = nullptr;
        _targetDevice = nullptr;
        _description = {};
        _heapSize = 0;
    }

    void Resize(UINT width, UINT height)
    {
        if (!_isInitialized || _description.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
        {
            throw std::logic_error("ERROR: Shared allocation cannot be resized before initialization or when it is a buffer. Initialize a "
                "Texture2D shared allocation before calling Resize().");
        }
        auto desc = _description;
        desc.Width = width;
        desc.Height = height;
        auto* source = _sourceDevice;
        auto* target = _targetDevice;
        ThrowIfFailed(Initialize(source, target, desc));
    }

    GDX12Resource* GetResource(GDX12Device* device)
    {
        if (!_isInitialized) { throw std::logic_error("ERROR: Shared allocation is not initialized. Call Initialize() before requesting a device mapping."); }
        if (device == _sourceDevice) { return &_sharedResourceSource; }
        if (device == _targetDevice) { return &_sharedResourceTarget; }
        throw std::invalid_argument("ERROR: Device does not map this shared allocation. Pass the source or target device used to initialize this "
            "allocation.");
    }
    UINT GetWidth() { return static_cast<UINT>(_description.Width); }
    UINT GetHeight() { return _description.Height; }
    DXGI_FORMAT GetFormat() { return _description.Format; }
    UINT64 GetHeapSize() const { return _heapSize; }
    const D3D12_RESOURCE_DESC& GetDescription() const { return _description; }
    bool IsInitialized() { return _isInitialized; }

private:
    GDX12Device* _sourceDevice = nullptr;
    GDX12Device* _targetDevice = nullptr;
    UINT64 _heapSize = 0;
    ComPtr<ID3D12Heap> _sharedHeap;
    ComPtr<ID3D12Heap> _openedHeap;
    GDX12TextureResource _sharedResourceSource;
    GDX12TextureResource _sharedResourceTarget;
    D3D12_RESOURCE_DESC _description = {};
    bool _isInitialized = false;
};
