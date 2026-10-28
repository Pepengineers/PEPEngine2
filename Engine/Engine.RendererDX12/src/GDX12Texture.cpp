#include "Engine.RendererDX12/GDX12Texture.h"

#include "Engine.RendererDX12/GDX12Device.h"
#include "Engine.RendererDX12/GDX12Descriptor.h"
#include "Engine.RendererDX12/GDX12DescriptorHeap.h"
#include "Engine.RendererDX12/GDX12TextureResource.h"

#include <stdexcept>

GDX12Texture::GDX12Texture() :
    _resourceFlags(D3D12_RESOURCE_FLAG_NONE), _isInitialized(false), _device(nullptr),
    _clearValue{}, _viewport{}, _scissorRect{}
{
}

GDX12Texture::~GDX12Texture() = default;

void GDX12Texture::Resize(UINT width, UINT height)
{
    if (_desc.ExternalResource)
    {
        const auto actual = _desc.ExternalResource->GetDesc();
        if (actual.Width != width || actual.Height != height)
        {
            throw std::runtime_error("ERROR: An external texture allocation cannot be resized by GDX12Texture. Wait for GPU completion and pass the "
                "resized allocation to ReplaceExternalResource().");
        }
    }
    _desc.Width = width;
    _desc.Height = height;
    ValidateDescription();
    if (!_desc.ExternalResource) { CreateResource(); }
    CreateViews();
    _viewport = { 0.f, 0.f, static_cast<FLOAT>(width), static_cast<FLOAT>(height), 0.f, 1.f };
    _scissorRect = { 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
}

void GDX12Texture::ReplaceExternalResource(ComPtr<ID3D12Resource> resource)
{
    if (!resource) { throw std::invalid_argument("ERROR: Given replacement external texture allocation is null. Pass a valid native texture allocation to "
        "ReplaceExternalResource()."); }
    auto desc = _desc;
    desc.ExternalResource = resource;
    const auto actual = resource->GetDesc();
    desc.Width = static_cast<UINT>(actual.Width);
    desc.Height = actual.Height;
    desc.ArraySize = actual.DepthOrArraySize;
    desc.MipLevels = actual.MipLevels;
    desc.SampleCount = actual.SampleDesc.Count;
    Initialize(desc);
}

const GDX12TextureDesc& GDX12Texture::GetDescription() const { return _desc; }
GDX12Device* GDX12Texture::GetDevice() const { return _device; }
GDX12Descriptor* GDX12Texture::GetSRV() { return _srv.get(); }
GDX12Descriptor* GDX12Texture::GetRTV() { return _rtv.get(); }
GDX12Descriptor* GDX12Texture::GetUAV() { return _uav.get(); }
GDX12Descriptor* GDX12Texture::GetDSV() { return _dsv.get(); }
bool GDX12Texture::HasDSV() { return _desc.CreateDSV; }
bool GDX12Texture::HasSRV() { return _desc.CreateSRV; }
bool GDX12Texture::HasUAV() { return _desc.CreateUAV; }
bool GDX12Texture::HasRTV() { return _desc.CreateRTV; }
D3D12_CLEAR_VALUE& GDX12Texture::GetClearValue() { return _clearValue; }
GDX12TextureResource* GDX12Texture::GetResource() { return _resource.get(); }
DXGI_FORMAT GDX12Texture::GetFormat() { return _desc.Format; }
ETextureSemantic GDX12Texture::GetSemantic() const { return _desc.Semantic; }
D3D12_VIEWPORT GDX12Texture::GetViewport() { return _viewport; }
D3D12_RECT GDX12Texture::GetScissorRect() { return _scissorRect; }
UINT GDX12Texture::GetWidth() { return _desc.Width; }
UINT GDX12Texture::GetHeight() { return _desc.Height; }
bool GDX12Texture::IsInitialized() { return _isInitialized; }

GDX12Descriptor* GDX12Texture::GetRTV(UINT layer)
{
    if (!_desc.CreateRTV || layer >= _desc.ArraySize) { throw std::out_of_range("ERROR: Requested texture RTV is not enabled or its layer is outside ArraySize. Create the texture with CreateRTV enabled and request a layer below ArraySize."); }
    if (_desc.ArraySize == 1) { return GetRTV(); }
    auto& view = _layerRTVs[layer];
    if (!view)
    {
        D3D12_RENDER_TARGET_VIEW_DESC desc = {};
        desc.Format = _desc.RTVDesc.Format;
        desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
        desc.Texture2DArray.FirstArraySlice = layer;
        desc.Texture2DArray.ArraySize = 1;
        view = std::make_unique<GDX12Descriptor>();
        view->InitAsRTV(_resource->D3DResource.Get(), &desc, _desc.RTVHeap, _desc.RTVHeap->GetAvailableIndex());
    }
    return view.get();
}

GDX12Descriptor* GDX12Texture::GetDSV(UINT layer)
{
    if (!_desc.CreateDSV || layer >= _desc.ArraySize) { throw std::out_of_range("ERROR: Requested texture DSV is not enabled or its layer is outside ArraySize. Create the texture with CreateDSV enabled and request a layer below ArraySize."); }
    if (_desc.ArraySize == 1) { return GetDSV(); }
    auto& view = _layerDSVs[layer];
    if (!view)
    {
        D3D12_DEPTH_STENCIL_VIEW_DESC desc = {};
        desc.Format = _desc.DSVDesc.Format;
        desc.Flags = _desc.DSVDesc.Flags;
        desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        desc.Texture2DArray.FirstArraySlice = layer;
        desc.Texture2DArray.ArraySize = 1;
        view = std::make_unique<GDX12Descriptor>();
        view->InitAsDSV(_resource->D3DResource.Get(), &desc, _desc.DSVHeap, _desc.DSVHeap->GetAvailableIndex());
    }
    return view.get();
}

GDX12Descriptor* GDX12Texture::GetReadOnlyDSV(UINT layer)
{
    if (!_desc.CreateDSV || layer >= _desc.ArraySize) { throw std::out_of_range("ERROR: Requested texture DSV is not enabled or its layer is outside ArraySize. Create the texture with CreateDSV enabled and request a layer below ArraySize."); }
    if (_desc.DSVDesc.Flags & D3D12_DSV_FLAG_READ_ONLY_DEPTH) { return GetDSV(layer); }
    auto& view = _readOnlyDSVs[layer];
    if (!view)
    {
        auto desc = _desc.DSVDesc;
        desc.Flags |= D3D12_DSV_FLAG_READ_ONLY_DEPTH;
        if (_desc.ArraySize > 1)
        {
            desc.Texture2DArray.FirstArraySlice = layer;
            desc.Texture2DArray.ArraySize = 1;
        }
        view = std::make_unique<GDX12Descriptor>();
        view->InitAsDSV(_resource->D3DResource.Get(), &desc, _desc.DSVHeap, _desc.DSVHeap->GetAvailableIndex());
    }
    return view.get();
}

void GDX12Texture::ValidateDescription()
{
    if (!_device || !_desc.Width || !_desc.Height || !_desc.ArraySize || !_desc.MipLevels ||
        _desc.ArraySize > D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION || _desc.SampleCount != 1 ||
        _desc.Width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION || _desc.Height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        _desc.Format == DXGI_FORMAT_UNKNOWN)
    {
        throw std::invalid_argument("ERROR: Given texture has an invalid device, dimensions, format, array size, mip count, or sample count. Use "
            "an initialized device, supported format, non-zero dimensions and array size, at least one mip, and "
            "SampleCount equal to one.");
    }
    if (_desc.IsCube && (_desc.Width != _desc.Height || _desc.ArraySize != 6))
    {
        throw std::invalid_argument("ERROR: Given TextureCube does not contain six square faces. Use TextureCube with ArraySize equal to six and "
            "matching non-zero width and height.");
    }
    UINT maxMipLevels = 1;
    for (UINT dimension = (std::max)(_desc.Width, _desc.Height); dimension > 1; dimension >>= 1) { ++maxMipLevels; }
    if (_desc.MipLevels > maxMipLevels) { throw std::invalid_argument("ERROR: Given texture mip count exceeds its complete mip-chain length. Reduce MipLevels to the mip-chain "
        "length supported by the texture dimensions."); }
    if (_desc.CreateDSV && (_desc.CreateRTV || _desc.CreateUAV))
    {
        throw std::invalid_argument("ERROR: Given depth texture also declares an RTV or UAV. Use depth SRV/DSV views and remove CreateRTV and "
            "CreateUAV from the texture description.");
    }
    auto validateHeap = [this](GDX12DescriptorHeap* heap, UINT index, D3D12_DESCRIPTOR_HEAP_TYPE type)
    {
        if (!heap || heap->_device != _device || heap->GetType() != type || index >= heap->GetNumDescriptors())
        {
            throw std::invalid_argument("ERROR: Given texture view has an invalid descriptor heap type, device, or index. Supply a descriptor heap of "
                "the correct type on the texture device and an index within that heap.");
        }
    };
    if (_desc.CreateSRV) { validateHeap(_desc.SRV_UAV_Heap, _desc.SRVHeapIndex, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV); }
    if (_desc.CreateUAV) { validateHeap(_desc.SRV_UAV_Heap, _desc.UAVHeapIndex, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV); }
    if (_desc.CreateRTV) { validateHeap(_desc.RTVHeap, _desc.RTVHeapIndex, D3D12_DESCRIPTOR_HEAP_TYPE_RTV); }
    if (_desc.CreateDSV) { validateHeap(_desc.DSVHeap, _desc.DSVHeapIndex, D3D12_DESCRIPTOR_HEAP_TYPE_DSV); }
    auto validateFormat = [this](DXGI_FORMAT format, D3D12_FORMAT_SUPPORT1 required, bool shaderRead)
    {
        if (format == DXGI_FORMAT_UNKNOWN || DirectX::IsTypeless(format))
        {
            throw std::invalid_argument("ERROR: Given texture view format is unknown or typeless. Use a concrete typed view format instead of "
                "DXGI_FORMAT_UNKNOWN or a typeless format.");
        }
        if (format != _desc.ResourceFormat &&
            (!DirectX::IsTypeless(_desc.ResourceFormat) || DirectX::MakeTypeless(format) != _desc.ResourceFormat))
        {
            throw std::invalid_argument("ERROR: Given texture view format is incompatible with its native resource format. Use a view format from the "
                "allocation format family.");
        }
        D3D12_FEATURE_DATA_FORMAT_SUPPORT support = { format };
        ThrowIfFailed(_device->GetDevice()->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support)));
        if ((support.Support1 & required) != required ||
            (shaderRead && !(support.Support1 & (D3D12_FORMAT_SUPPORT1_SHADER_LOAD | D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE))))
        {
            throw std::invalid_argument("ERROR: Given device does not support the requested view usage for this texture format. Use a format "
                "supporting the declared view usage or remove the unsupported view.");
        }
    };
    if (_desc.CreateSRV) { validateFormat(_desc.SRVDesc.Format, D3D12_FORMAT_SUPPORT1_TEXTURE2D, true); }
    if (_desc.CreateRTV) { validateFormat(_desc.RTVDesc.Format, D3D12_FORMAT_SUPPORT1_RENDER_TARGET, false); }
    if (_desc.CreateDSV) { validateFormat(_desc.DSVDesc.Format, D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL, false); }
    if (_desc.CreateUAV) { validateFormat(_desc.UAVDesc.Format, D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW, false); }
    if (_desc.ExternalResource)
    {
        const auto actual = _desc.ExternalResource->GetDesc();
        if (actual.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || actual.Width != _desc.Width ||
            actual.Height != _desc.Height || actual.DepthOrArraySize != _desc.ArraySize ||
            actual.MipLevels != _desc.MipLevels || actual.SampleDesc.Count != _desc.SampleCount ||
            (actual.Flags & _resourceFlags) != _resourceFlags ||
            (_desc.CreateSRV && (actual.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)))
        {
            throw std::invalid_argument("ERROR: Given external texture allocation does not match its declared description or views. Match the declared "
                "dimensions, layers, mip count, samples, format, and view flags to the external allocation.");
        }
        ComPtr<ID3D12Device> owner;
        ThrowIfFailed(_desc.ExternalResource->GetDevice(IID_PPV_ARGS(&owner)));
        ComPtr<IUnknown> ownerIdentity;
        ComPtr<IUnknown> deviceIdentity;
        ThrowIfFailed(owner.As(&ownerIdentity));
        ThrowIfFailed(_device->GetDevice().As(&deviceIdentity));
        if (ownerIdentity.Get() != deviceIdentity.Get())
        {
            throw std::invalid_argument("ERROR: Given external texture allocation belongs to another device. Use descriptor heaps and an external "
                "allocation from the same GDX12Device.");
        }
    }
}

void GDX12Texture::CreateResource()
{
    const auto resourceDesc = CD3DX12_RESOURCE_DESC::Tex2D(
        _desc.ResourceFormat, _desc.Width, _desc.Height, static_cast<UINT16>(_desc.ArraySize),
        static_cast<UINT16>(_desc.MipLevels), _desc.SampleCount, 0, _resourceFlags);
    const CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
    ComPtr<ID3D12Resource> resource;
    ThrowIfFailed(_device->GetDevice()->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE,
        &resourceDesc, D3D12_RESOURCE_STATE_COMMON,
        (_desc.CreateRTV || _desc.CreateDSV) ? &_clearValue : nullptr, IID_PPV_ARGS(&resource)));
    _resource = std::make_unique<GDX12TextureResource>(resource);
    _resource->ResetState();
}

void GDX12Texture::CreateViews()
{
    _layerRTVs.clear();
    _layerDSVs.clear();
    _readOnlyDSVs.clear();
    _layerRTVs.resize(_desc.ArraySize);
    _layerDSVs.resize(_desc.ArraySize);
    _readOnlyDSVs.resize(_desc.ArraySize);
    if (_desc.CreateSRV)
    {
        if (!_srv) { _srv = std::make_unique<GDX12Descriptor>(); }
        _srv->InitAsSRV(_resource->D3DResource.Get(), &_desc.SRVDesc, _desc.SRV_UAV_Heap, _desc.SRVHeapIndex);
    }
    else { _srv.reset(); }
    if (_desc.CreateUAV)
    {
        if (!_uav) { _uav = std::make_unique<GDX12Descriptor>(); }
        _uav->InitAsUAV(_resource->D3DResource.Get(), nullptr, &_desc.UAVDesc, _desc.SRV_UAV_Heap, _desc.UAVHeapIndex);
    }
    else { _uav.reset(); }
    if (_desc.CreateRTV)
    {
        if (!_rtv) { _rtv = std::make_unique<GDX12Descriptor>(); }
        _rtv->InitAsRTV(_resource->D3DResource.Get(), &_desc.RTVDesc, _desc.RTVHeap, _desc.RTVHeapIndex);
    }
    else { _rtv.reset(); }
    if (_desc.CreateDSV)
    {
        if (!_dsv) { _dsv = std::make_unique<GDX12Descriptor>(); }
        _dsv->InitAsDSV(_resource->D3DResource.Get(), &_desc.DSVDesc, _desc.DSVHeap, _desc.DSVHeapIndex);
    }
    else { _dsv.reset(); }
}

void GDX12Texture::Initialize(GDX12TextureDesc desc)
{
    _isInitialized = false;
    _desc = desc;
    _device = desc.Device;
    if (!_device && desc.SRV_UAV_Heap) { _device = desc.SRV_UAV_Heap->_device; }
    if (!_device && desc.RTVHeap) { _device = desc.RTVHeap->_device; }
    if (!_device && desc.DSVHeap) { _device = desc.DSVHeap->_device; }
    if (desc.ExternalResource)
    {
        const auto actual = desc.ExternalResource->GetDesc();
        _desc.ArraySize = actual.DepthOrArraySize;
        _desc.MipLevels = actual.MipLevels;
        _desc.SampleCount = actual.SampleDesc.Count;
        _desc.ResourceFormat = actual.Format;
    }
    if (_desc.ResourceFormat == DXGI_FORMAT_UNKNOWN)
    {
        _desc.ResourceFormat = _desc.CreateSRV && _desc.Format == DXGI_FORMAT_D32_FLOAT
            ? DXGI_FORMAT_R32_TYPELESS : _desc.Format;
    }
    _resourceFlags = D3D12_RESOURCE_FLAG_NONE;
    if (_desc.CreateRTV) { _resourceFlags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET; }
    if (_desc.CreateDSV) { _resourceFlags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL; }
    if (_desc.CreateUAV) { _resourceFlags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS; }

    if (_desc.CreateSRV)
    {
        auto& view = _desc.SRVDesc;
        if (view.Format == DXGI_FORMAT_UNKNOWN || view.Format == DXGI_FORMAT_D32_FLOAT)
        {
            view.Format = _desc.Format == DXGI_FORMAT_D32_FLOAT ? DXGI_FORMAT_R32_FLOAT : _desc.Format;
        }
        if (!view.Shader4ComponentMapping) { view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; }
        if (_desc.IsCube)
        {
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
            view.TextureCube = {};
            view.TextureCube.MipLevels = _desc.MipLevels;
        }
        else if (_desc.ArraySize > 1 && (view.ViewDimension == D3D12_SRV_DIMENSION_UNKNOWN || view.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D))
        {
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            view.Texture2DArray = {};
            view.Texture2DArray.ArraySize = _desc.ArraySize;
            view.Texture2DArray.MipLevels = _desc.MipLevels;
        }
        else if (view.ViewDimension == D3D12_SRV_DIMENSION_UNKNOWN)
        {
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            view.Texture2D.MipLevels = _desc.MipLevels;
        }
    }
    if (_desc.CreateRTV)
    {
        auto& view = _desc.RTVDesc;
        if (view.Format == DXGI_FORMAT_UNKNOWN) { view.Format = _desc.Format; }
        if (_desc.ArraySize > 1)
        {
            view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
            view.Texture2DArray = {};
            view.Texture2DArray.ArraySize = _desc.ArraySize;
        }
        else if (view.ViewDimension == D3D12_RTV_DIMENSION_UNKNOWN) { view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D; }
    }
    if (_desc.CreateDSV)
    {
        auto& view = _desc.DSVDesc;
        if (view.Format == DXGI_FORMAT_UNKNOWN) { view.Format = _desc.Format; }
        if (_desc.ArraySize > 1)
        {
            view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
            view.Texture2DArray = {};
            view.Texture2DArray.ArraySize = _desc.ArraySize;
        }
        else if (view.ViewDimension == D3D12_DSV_DIMENSION_UNKNOWN) { view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D; }
    }
    if (_desc.CreateUAV)
    {
        auto& view = _desc.UAVDesc;
        if (view.Format == DXGI_FORMAT_UNKNOWN) { view.Format = _desc.Format; }
        if (_desc.ArraySize > 1)
        {
            view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
            view.Texture2DArray = {};
            view.Texture2DArray.ArraySize = _desc.ArraySize;
        }
        else if (view.ViewDimension == D3D12_UAV_DIMENSION_UNKNOWN) { view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D; }
    }
    _clearValue = {};
    _clearValue.Format = _desc.CreateDSV ? _desc.DSVDesc.Format : _desc.RTVDesc.Format;
    if (_desc.CreateDSV)
    {
        _clearValue.DepthStencil.Depth = _desc.ClearValue.x;
        _clearValue.DepthStencil.Stencil = static_cast<UINT8>(_desc.ClearValue.y);
    }
    else
    {
        _clearValue.Color[0] = _desc.ClearValue.x;
        _clearValue.Color[1] = _desc.ClearValue.y;
        _clearValue.Color[2] = _desc.ClearValue.z;
        _clearValue.Color[3] = _desc.ClearValue.w;
    }
    ValidateDescription();
    if (_desc.ExternalResource) { _resource = std::make_unique<GDX12TextureResource>(_desc.ExternalResource); }
    else { CreateResource(); }
    CreateViews();
    _viewport = { 0.f, 0.f, static_cast<FLOAT>(_desc.Width), static_cast<FLOAT>(_desc.Height), 0.f, 1.f };
    _scissorRect = { 0, 0, static_cast<LONG>(_desc.Width), static_cast<LONG>(_desc.Height) };
    _isInitialized = true;
}
