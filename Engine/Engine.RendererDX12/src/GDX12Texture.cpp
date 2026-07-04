#include "Engine.RendererDX12/GDX12Texture.h"

#include "Engine.RendererDX12/GDX12Device.h"
#include "Engine.RendererDX12/GDX12Descriptor.h"
#include "Engine.RendererDX12/GDX12DescriptorHeap.h"
#include "Engine.RendererDX12/GDX12CommandList.h"
#include "Engine.RendererDX12/GDX12TextureResource.h"

GDX12Texture::GDX12Texture() :
    _srv(nullptr), _rtv(nullptr), _uav(nullptr), _dsv(nullptr),
    _resourceFlags(D3D12_RESOURCE_FLAG_NONE), _isInitialized(false), _device(nullptr)
{
}

void GDX12Texture::Resize(UINT width, UINT height)
{
    _desc.Width = width;
    _desc.Height = height;
    if (_desc.ExternalResource == nullptr) { CreateResource(); }
    CreateViews();

    _viewport.TopLeftX = 0;
    _viewport.TopLeftY = 0;
    _viewport.Width = static_cast<FLOAT>(width);
    _viewport.Height = static_cast<FLOAT>(height);
    _viewport.MinDepth = 0.0f;
    _viewport.MaxDepth = 1.0f;

    _scissorRect = { 0, 0, static_cast<int>(width), static_cast<int>(height) };
}

GDX12Descriptor* GDX12Texture::GetSRV()
{
    if (!_desc.CreateSRV) { OutputDebugStringA("ERROR: Can't get Texture SRV: SRV not created.\n"); }
    return _srv.get();
}

GDX12Descriptor* GDX12Texture::GetRTV()
{
    if (!_desc.CreateRTV) { OutputDebugStringA("ERROR: Can't get Texture RTV: RTV not created.\n"); }
    return _rtv.get();
}

GDX12Descriptor* GDX12Texture::GetUAV()
{
    if (!_desc.CreateUAV) { OutputDebugStringA("ERROR: Can't get Texture UAV: UAV not created.\n"); }
    return _uav.get();
}

GDX12Descriptor* GDX12Texture::GetDSV()
{
    if (!_desc.CreateDSV) { OutputDebugStringA("ERROR: Can't get Texture DSV: DSV not created.\n"); }
    return _dsv.get();
}

D3D12_CLEAR_VALUE& GDX12Texture::GetClearValue()
{
    return _clearValue;
}

GDX12TextureResource* GDX12Texture::GetResource()
{
    return _resource.get();
}

DXGI_FORMAT GDX12Texture::GetFormat()
{
    return _desc.Format;
}

ETextureSemantic GDX12Texture::GetSemantic() const
{
    return _desc.Semantic;
}

D3D12_VIEWPORT GDX12Texture::GetViewport()
{
    return _viewport;
}

D3D12_RECT GDX12Texture::GetScissorRect()
{
    return _scissorRect;
}

UINT GDX12Texture::GetWidth()
{
    return _desc.Width;
}

UINT GDX12Texture::GetHeight()
{
    return _desc.Height;
}

GDX12Texture* GDX12Texture::GetTexture()
{
    return this;
}

bool GDX12Texture::IsInitialized()
{
    return _isInitialized;
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

    ComPtr<ID3D12Resource> d3dResource;
    _device->GetDevice()->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &resourceDesc,
        D3D12_RESOURCE_STATE_COMMON,
        (_desc.CreateRTV || _desc.CreateDSV) ? &_clearValue : nullptr,
        IID_PPV_ARGS(&d3dResource));

    if (!_resource) { _resource = std::make_unique<GDX12TextureResource>(d3dResource); }
    else { _resource->D3DResource = d3dResource; }
    _resource->ResetState();
}

void GDX12Texture::CreateViews()
{
    if (_desc.CreateSRV)
    {
        if (!_desc.SRV_UAV_Heap) { OutputDebugStringA("ERROR: No Texture SRV_UAV Heap specified\n"); }
        if (_desc.SRVHeapIndex == -1) { OutputDebugStringA("ERROR: No Texture SRVHeapIndex specified\n"); }

        if(!_srv) _srv = std::make_unique<GDX12Descriptor>();
        _srv->InitAsSRV(_resource->D3DResource.Get(), &_desc.SRVDesc, _desc.SRV_UAV_Heap, _desc.SRVHeapIndex);
    }

    if (_desc.CreateUAV)
    {
        if (!_desc.SRV_UAV_Heap) { OutputDebugStringA("ERROR: No Texture SRV_UAV Heap specified\n"); }
        if (_desc.UAVHeapIndex == -1) { OutputDebugStringA("ERROR: No Texture UAVHeapIndex specified\n"); }

        if (!_uav) _uav = std::make_unique<GDX12Descriptor>();
        _uav->InitAsUAV(_resource->D3DResource.Get(), nullptr, &_desc.UAVDesc, _desc.SRV_UAV_Heap, _desc.UAVHeapIndex);
    }

    if (_desc.CreateRTV)
    {
        if (!_desc.RTVHeap) { OutputDebugStringA("ERROR: No Texture RTV Heap specified\n"); }
        if (_desc.RTVHeapIndex == -1) { OutputDebugStringA("ERROR: No Texture RTVHeapIndex specified\n"); }

        if (!_rtv) _rtv = std::make_unique<GDX12Descriptor>();
        _rtv->InitAsRTV(_resource->D3DResource.Get(), &_desc.RTVDesc, _desc.RTVHeap, _desc.RTVHeapIndex);
    }

    if (_desc.CreateDSV)
    {
        if (!_desc.DSVHeap) { OutputDebugStringA("ERROR: No Texture DSV Heap specified\n"); }
        if (_desc.DSVHeapIndex == -1) { OutputDebugStringA("ERROR: No Texture DSVHeapIndex specified\n"); }

        if (!_dsv) _dsv = std::make_unique<GDX12Descriptor>();
        _dsv->InitAsDSV(_resource->D3DResource.Get(), &_desc.DSVDesc, _desc.DSVHeap, _desc.DSVHeapIndex);
    }
}

GDX12Texture::~GDX12Texture()
{
    _resource.reset();
    _srv.reset();
    _rtv.reset();
    _uav.reset();
    _dsv.reset();
}

void GDX12Texture::Initialize(GDX12TextureDesc desc)
{
    _desc = desc;
    if (_desc.CreateRTV && _desc.CreateDSV) { OutputDebugStringA("ERROR: It is impossible to create RTV and DSV on the same texture.\n"); }
    if (_desc.CreateUAV && _desc.CreateDSV) { OutputDebugStringA("ERROR: It is impossible to create DSV and UAV on the same texture.\n"); }

    //grab device pointer from any avalible heap
    if (_desc.SRV_UAV_Heap) { _device = _desc.SRV_UAV_Heap->_device; }
    if (_desc.RTVHeap) { _device = _desc.RTVHeap->_device; }
    if (_desc.DSVHeap) { _device = _desc.DSVHeap->_device; }

    if (_desc.CreateRTV) { _resourceFlags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS; }
    if (_desc.CreateDSV) { _resourceFlags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL; }
    if (_desc.CreateUAV) { _resourceFlags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS; }
    if (_desc.CreateSRV) { _resourceFlags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS; }

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

    if (_desc.ExternalResource != nullptr) { _resource = std::make_unique<GDX12TextureResource>(_desc.ExternalResource); }
    else { CreateResource(); }

    CreateViews();

    _viewport.TopLeftX = 0;
    _viewport.TopLeftY = 0;
    _viewport.Width = static_cast<FLOAT>(_desc.Width);
    _viewport.Height = static_cast<FLOAT>(_desc.Height);
    _viewport.MinDepth = 0.0f;
    _viewport.MaxDepth = 1.0f;

    _scissorRect = { 0, 0, static_cast<int>(_desc.Width), static_cast<int>(_desc.Height) };

    _isInitialized = true;
}
