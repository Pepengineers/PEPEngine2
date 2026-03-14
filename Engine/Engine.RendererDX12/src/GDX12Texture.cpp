#include "Engine.RendererDX12/GDX12Texture.h"

#include "Engine.RendererDX12/GDX12Device.h"
#include "Engine.RendererDX12/GDX12Descriptor.h"
#include "Engine.RendererDX12/GDX12DescriptorHeap.h"
#include "Engine.RendererDX12/GDX12CommandList.h"

GDX12Texture::GDX12Texture(GDX12TextureDesc desc) :
    _desc(desc),
    _srv(nullptr),
    _rtv(nullptr),
    _uav(nullptr),
    _dsv(nullptr),
    _resourceFlags(D3D12_RESOURCE_FLAG_NONE),
    _currentLayout(D3D12_BARRIER_LAYOUT_COMMON),
    _currentAccess(D3D12_BARRIER_ACCESS_COMMON),
    _currentSync(D3D12_BARRIER_SYNC_NONE)
{
    if (_desc.CreateRTV && _desc.CreateDSV) { OutputDebugStringA("ERROR: It is impossible to create RTV and DSV on the same texture."); }
    if (_desc.CreateUAV && _desc.CreateDSV) { OutputDebugStringA("ERROR: It is impossible to create DSV and UAV on the same texture."); }

    //grab device pointer from any avalible heap
    if (_desc.SRV_UAV_Heap) { _device = _desc.SRV_UAV_Heap->_device; }
    if (_desc.RTVHeap) { _device = _desc.RTVHeap->_device; }
    if (_desc.DSVHeap) { _device = _desc.DSVHeap->_device; }

    if (_desc.CreateRTV) { _resourceFlags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET; }
    if (_desc.CreateDSV) { _resourceFlags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL; }
    if (_desc.CreateUAV) { _resourceFlags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS; }

    if (_desc.CreateRTV)
    {
        _clearValue.Color[0] = _desc.ClearValue.x;
        _clearValue.Color[1] = _desc.ClearValue.y;
        _clearValue.Color[2] = _desc.ClearValue.z;
        _clearValue.Color[3] = _desc.ClearValue.w;
    }
    else if (_desc.CreateDSV)
    {
        _clearValue.DepthStencil.Depth = _desc.ClearValue.x;
        _clearValue.DepthStencil.Stencil = _desc.ClearValue.y;
    }

    _clearValue.Format = _desc.Format;

    if (_desc.ExternalResource != nullptr) { _resource = _desc.ExternalResource; }
    else { CreateResource(); }
    
    CreateViews();
}

void GDX12Texture::Resize(UINT Width, UINT Height)
{
    if (_desc.ExternalResource != nullptr) { CreateResource(); }
    CreateViews();
}

GDX12Descriptor* GDX12Texture::GetSRV()
{
    if (!_desc.CreateSRV) { OutputDebugStringA("ERROR: Can't get Texture SRV: SRV not created."); }
    return _srv.get();
}

GDX12Descriptor* GDX12Texture::GetRTV()
{
    if (!_desc.CreateRTV) { OutputDebugStringA("ERROR: Can't get Texture RTV: RTV not created."); }
    return _rtv.get();
}

GDX12Descriptor* GDX12Texture::GetUAV()
{
    if (!_desc.CreateUAV) { OutputDebugStringA("ERROR: Can't get Texture UAV: UAV not created."); }
    return _uav.get();
}

GDX12Descriptor* GDX12Texture::GetDSV()
{
    if (!_desc.CreateDSV) { OutputDebugStringA("ERROR: Can't get Texture DSV: DSV not created."); }
    return _dsv.get();
}

D3D12_CLEAR_VALUE& GDX12Texture::GetClearValue()
{
    return _clearValue;
}

ComPtr<ID3D12Resource> GDX12Texture::GetD3DResource()
{
    return _resource;
}

DXGI_FORMAT GDX12Texture::GetFormat()
{
    return _desc.Format;
}

void GDX12Texture::CreateResource()
{
    D3D12_RESOURCE_DESC resourceDesc = CD3DX12_RESOURCE_DESC::Tex2D(
        _desc.Format, _desc.Width, _desc.Height,
        1,  // array size
        1,  // mip levels
        1,  // sample count
        0,  // sample quality
        _resourceFlags);
    
    CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);

    HRESULT hr = _device->GetDevice()->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &resourceDesc,
        D3D12_RESOURCE_STATE_COMMON,
        (_desc.CreateRTV || _desc.CreateDSV) ? &_clearValue : nullptr,
        IID_PPV_ARGS(&_resource));

    if (FAILED(hr)) 
    {
        char msg[256];
        sprintf_s(msg, "ERROR: Texture resource creation failed with HRESULT: 0x%08X\n", hr);
        OutputDebugStringA(msg);
    }
}

void GDX12Texture::CreateViews()
{
    if (_desc.CreateSRV)
    {
        if (!_desc.SRV_UAV_Heap) { OutputDebugStringA("ERROR: No Texture SRV_UAV Heap specified"); }
        _srv = std::make_unique<GDX12Descriptor>();
        _srv->InitAsSRV(_resource.Get(), &_desc.SRVDesc, _desc.SRV_UAV_Heap);
    }

    if (_desc.CreateUAV)
    {
        if (!_desc.SRV_UAV_Heap) { OutputDebugStringA("ERROR: No Texture SRV_UAV Heap specified"); }
        _uav = std::make_unique<GDX12Descriptor>();
        _uav->InitAsUAV(_resource.Get(), &_desc.UAVDesc, _desc.SRV_UAV_Heap);
    }

    if (_desc.CreateRTV)
    {
        if (!_desc.RTVHeap) { OutputDebugStringA("ERROR: No Texture RTV Heap specified"); }
        _rtv = std::make_unique<GDX12Descriptor>();
        _rtv->InitAsRTV(_resource.Get(), &_desc.RTVDesc, _desc.RTVHeap);
    }

    if (_desc.CreateDSV)
    {
        if (!_desc.DSVHeap) { OutputDebugStringA("ERROR: No Texture DSV Heap specified"); }
        _dsv = std::make_unique<GDX12Descriptor>();
        _dsv->InitAsDSV(_resource.Get(), &_desc.DSVDesc, _desc.DSVHeap);
    }
}

D3D12_TEXTURE_BARRIER GDX12Texture::CreateBarrier(D3D12_BARRIER_SYNC syncAfter, D3D12_BARRIER_ACCESS accessAfter, D3D12_BARRIER_LAYOUT layoutAfter)
{
    D3D12_TEXTURE_BARRIER barrier = {};

    if (_currentSync == D3D12_BARRIER_SYNC_NONE && _currentAccess == D3D12_BARRIER_ACCESS_COMMON)
    {
        barrier.SyncBefore = D3D12_BARRIER_SYNC_ALL;
    }
    else
    {
        barrier.SyncBefore = _currentSync;
    }

    barrier.SyncAfter = syncAfter;
    barrier.AccessBefore = _currentAccess;
    barrier.AccessAfter = accessAfter;
    barrier.LayoutBefore = _currentLayout;
    barrier.LayoutAfter = layoutAfter;
    barrier.pResource = _resource.Get();

    // All subresources
    barrier.Subresources.IndexOrFirstMipLevel = 0;
    barrier.Subresources.NumMipLevels = 1;
    barrier.Subresources.FirstArraySlice = 0;
    barrier.Subresources.NumArraySlices = 1;
    barrier.Subresources.FirstPlane = 0;
    barrier.Subresources.NumPlanes = 1;

    barrier.Flags = D3D12_TEXTURE_BARRIER_FLAG_NONE;

    return barrier;
}

GDX12Texture::~GDX12Texture()
{
    _resource.Reset();
    _srv.reset();
    _rtv.reset();
    _uav.reset();
    _dsv.reset();
}
