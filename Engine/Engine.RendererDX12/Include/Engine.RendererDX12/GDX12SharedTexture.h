#pragma once

#include "Engine.RendererDX12\D3DHelpers.h"

#include "Engine.Core/Types/TextureTypes.h"
#include "Engine.RendererDX12/IRenderPassLink.h"
#include "Engine.RendererDX12/GDX12Device.h"
#include "Engine.RendererDX12/GDX12TextureResource.h"

class GDX12SharedTexture : public IRenderPassLink
{
public:
    GDX12SharedTexture() : _width(0) , _height(0),
		_format(DXGI_FORMAT_UNKNOWN), _heapSize(0), _transferFromDeivce(nullptr), 
        _transferToDevice(nullptr), _isInitiliazed(false)
    {
    }

	~GDX12SharedTexture()
	{
		Release();
	}

    GDX12Texture* GetTexture() override
    {
        OutputDebugStringA("ERROR: Getting texture from SharedTexture via interface is not allowed! Use GetSharedTexture() or get it via CopyFromRenderPass.\n");
        return nullptr;
    }

    void Initialize(GDX12Device* transferFromDeivce, GDX12Device* transferToDevice,
        UINT width, UINT height, DXGI_FORMAT format)
    {
        Release();

        _transferFromDeivce = transferFromDeivce;
        _transferToDevice = transferToDevice;
        _width = width;
        _height = height;
        _format = format;

        CreateSharedHeap();
        CreatePlacedResources();
        ShareResources();

        _isInitiliazed = true;
    }

    void Release()
    {
        _sharedTexturePrimary.Reset();
        _sharedTextureSecondary.Reset();
        _sharedHeap.Reset();
        _width = 0;
        _height = 0;
        _format = DXGI_FORMAT_UNKNOWN;
		_heapSize = 0;
    }

    void Resize(UINT width, UINT height)
    {
        DXGI_FORMAT cachedFormat = _format;
        Release();
        Initialize(_transferFromDeivce, _transferToDevice, width, height, cachedFormat);
    }

    //Transfer happens from primary to secondary
    GDX12Resource* GetPrimaryResource() { return &_sharedTexturePrimary; }
    GDX12Resource* GetSecondaryResource() { return &_sharedTextureSecondary; }
    UINT GetWidth() { return _width; }
    UINT GetHeight() { return _height; }
    DXGI_FORMAT GetFormat() { return _format; }
    GDX12SharedTexture* GetSharedTexture() override { return this; }
    bool IsInitialized() override { return _isInitiliazed; }

private:
	GDX12Device* _transferFromDeivce;
	GDX12Device* _transferToDevice;
	UINT64 _heapSize;
	ComPtr<ID3D12Heap> _sharedHeap;
	GDX12TextureResource _sharedTexturePrimary;
	GDX12TextureResource _sharedTextureSecondary;
	DXGI_FORMAT _format;
	UINT _width;
	UINT _height;
    bool _isInitiliazed;

    void CreateSharedHeap()
    {
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Alignment = 0;
        desc.Width = _width;
        desc.Height = _height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = _format;
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;

        D3D12_RESOURCE_ALLOCATION_INFO primaryInfo = _transferFromDeivce->GetDevice()->GetResourceAllocationInfo(0, 1, &desc);
        D3D12_RESOURCE_ALLOCATION_INFO secondaryInfo = _transferToDevice->GetDevice()->GetResourceAllocationInfo(0, 1, &desc);

        _heapSize = max(primaryInfo.SizeInBytes, secondaryInfo.SizeInBytes);
        _heapSize = (_heapSize + D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT - 1) & ~(D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT - 1);

        D3D12_HEAP_DESC heapDesc = {};
        heapDesc.SizeInBytes = _heapSize;
        heapDesc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        heapDesc.Properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        heapDesc.Properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        heapDesc.Properties.CreationNodeMask = 1;
        heapDesc.Properties.VisibleNodeMask = 1;
        heapDesc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
        heapDesc.Flags = D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER;

        ThrowIfFailed(_transferFromDeivce->GetDevice()->CreateHeap(
            &heapDesc, IID_PPV_ARGS(&_sharedHeap)),
            "Failed to create shared heap for cross-adapter texture");
    }

    void CreatePlacedResources()
    {
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Alignment = 0;
        desc.Width = _width;
        desc.Height = _height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = _format;
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;

        ThrowIfFailed(_transferFromDeivce->GetDevice()->CreatePlacedResource(
            _sharedHeap.Get(), 0, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
            IID_PPV_ARGS(&_sharedTexturePrimary.D3DResource)), "Failed to create placed resource on primary device");
    }

    void ShareResources()
    {
        HANDLE heapHandle = nullptr;
        ThrowIfFailed(_transferFromDeivce->GetDevice()->CreateSharedHandle(
            _sharedHeap.Get(), nullptr, GENERIC_ALL, nullptr,
            &heapHandle), "Failed to create shared handle for heap");

        if (!heapHandle) throw std::runtime_error("Failed to create shared handle (handle is null)");

        Microsoft::WRL::ComPtr<ID3D12Heap> sharedHeapOnSecondary;
        HRESULT hr = _transferToDevice->GetDevice()->OpenSharedHandle(
            heapHandle,
            IID_PPV_ARGS(&sharedHeapOnSecondary));

        CloseHandle(heapHandle);
        ThrowIfFailed(hr, "Failed to open shared heap handle on secondary device");

        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Alignment = 0;
        desc.Width = _width;
        desc.Height = _height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = _format;
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;

        ThrowIfFailed(_transferToDevice->GetDevice()->CreatePlacedResource(
            sharedHeapOnSecondary.Get(), 0, &desc, D3D12_RESOURCE_STATE_COMMON,
            nullptr, IID_PPV_ARGS(&_sharedTextureSecondary.D3DResource)),
            "Failed to create placed resource on secondary device");
    }
};