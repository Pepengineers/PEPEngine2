#include "Engine.RendererDX12/GDX12TextureResource.h"

GDX12TextureResource::GDX12TextureResource(ComPtr<ID3D12Resource> resource) :
	D3DResource(resource),
	_currentSync(D3D12_BARRIER_SYNC_NONE),
	_currentAccess(D3D12_BARRIER_ACCESS_COMMON),
	_currentLayout(D3D12_BARRIER_LAYOUT_COMMON)
{
	_desc = D3DResource->GetDesc();
}

GDX12TextureResource::~GDX12TextureResource()
{
}

D3D12_TEXTURE_BARRIER GDX12TextureResource::GetBarrier(
    D3D12_BARRIER_SYNC syncAfter,
    D3D12_BARRIER_ACCESS accessAfter,
    D3D12_BARRIER_LAYOUT layoutAfter)
{
    auto barrier = CreateBarrier(syncAfter, accessAfter, layoutAfter);
    SetCurrentState(syncAfter, accessAfter, layoutAfter);
    return barrier;
}

void GDX12TextureResource::SetCurrentState(D3D12_BARRIER_SYNC sync, D3D12_BARRIER_ACCESS access, D3D12_BARRIER_LAYOUT layout)
{
    _currentSync = sync;
    _currentAccess = access;
    _currentLayout = layout;
}

D3D12_BARRIER_SYNC GDX12TextureResource::GetCurrentSync()
{
	return _currentSync;
}

D3D12_BARRIER_ACCESS GDX12TextureResource::GetCurrentAccess()
{
	return _currentAccess;
}

D3D12_BARRIER_LAYOUT GDX12TextureResource::GetCurrentLayout()
{
	return _currentLayout;;
}

D3D12_TEXTURE_BARRIER GDX12TextureResource::CreateBarrier(D3D12_BARRIER_SYNC syncAfter, D3D12_BARRIER_ACCESS accessAfter, D3D12_BARRIER_LAYOUT layoutAfter)
{
    D3D12_TEXTURE_BARRIER barrier = {};

    if (_currentSync == D3D12_BARRIER_SYNC_NONE && _currentAccess == D3D12_BARRIER_ACCESS_COMMON)
    {
        barrier.SyncBefore = D3D12_BARRIER_SYNC_ALL;
    }
    else { barrier.SyncBefore = _currentSync; }

    barrier.SyncAfter = syncAfter;
    barrier.AccessBefore = _currentAccess;
    barrier.AccessAfter = accessAfter;
    barrier.LayoutBefore = _currentLayout;
    barrier.LayoutAfter = layoutAfter;
    barrier.pResource = D3DResource.Get();

    //All subresources
    barrier.Subresources.IndexOrFirstMipLevel = 0;
    barrier.Subresources.NumMipLevels = _desc.MipLevels;
    barrier.Subresources.FirstArraySlice = 0;
    barrier.Subresources.NumArraySlices = _desc.DepthOrArraySize;
    barrier.Subresources.FirstPlane = 0;
    barrier.Subresources.NumPlanes = 1;

    barrier.Flags = D3D12_TEXTURE_BARRIER_FLAG_NONE;

    return barrier;
}

D3D12_TEXTURE_BARRIER GDX12TextureResource::GetRenderTargetBarrier()
{
    return GetBarrier(D3D12_BARRIER_SYNC_RENDER_TARGET, D3D12_BARRIER_ACCESS_RENDER_TARGET,
        D3D12_BARRIER_LAYOUT_RENDER_TARGET);
}

D3D12_TEXTURE_BARRIER GDX12TextureResource::GetPixelShaderResourceBarrier()
{
    return GetBarrier(D3D12_BARRIER_SYNC_PIXEL_SHADING, D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
        D3D12_BARRIER_LAYOUT_SHADER_RESOURCE);
}

D3D12_TEXTURE_BARRIER GDX12TextureResource::GetNonPixelShaderResourceBarrier()
{
    return GetBarrier(D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
        D3D12_BARRIER_LAYOUT_SHADER_RESOURCE);
}

D3D12_TEXTURE_BARRIER GDX12TextureResource::GetUnorderedAccessBarrier()
{
    return GetBarrier(D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
        D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
}

D3D12_TEXTURE_BARRIER GDX12TextureResource::GetCopyDestBarrier()
{
    return GetBarrier(D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_ACCESS_COPY_DEST,
        D3D12_BARRIER_LAYOUT_COPY_DEST);
}

D3D12_TEXTURE_BARRIER GDX12TextureResource::GetCopySourceBarrier()
{
    return GetBarrier(D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_ACCESS_COPY_SOURCE,
        D3D12_BARRIER_LAYOUT_COPY_SOURCE);
}

D3D12_TEXTURE_BARRIER GDX12TextureResource::GetPresentBarrier()
{
    return GetBarrier(D3D12_BARRIER_SYNC_ALL, D3D12_BARRIER_ACCESS_COMMON, D3D12_BARRIER_LAYOUT_PRESENT);
}

D3D12_TEXTURE_BARRIER GDX12TextureResource::GetCommonBarrier()
{
    return GetBarrier(D3D12_BARRIER_SYNC_ALL, D3D12_BARRIER_ACCESS_COMMON,
        D3D12_BARRIER_LAYOUT_COMMON);
}

D3D12_TEXTURE_BARRIER GDX12TextureResource::GetDepthWriteBarrier()
{
    return GetBarrier(D3D12_BARRIER_SYNC_DEPTH_STENCIL, D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE,
        D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE);
}

D3D12_TEXTURE_BARRIER GDX12TextureResource::GetDepthReadBarrier()
{
    return GetBarrier(D3D12_BARRIER_SYNC_DEPTH_STENCIL, D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ, 
        D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_READ);
}

D3D12_TEXTURE_BARRIER GDX12TextureResource::GetResolveSourceBarrier()
{
    return GetBarrier(D3D12_BARRIER_SYNC_RESOLVE, D3D12_BARRIER_ACCESS_RESOLVE_SOURCE, 
        D3D12_BARRIER_LAYOUT_RESOLVE_SOURCE);
}

D3D12_TEXTURE_BARRIER GDX12TextureResource::GetResolveDestBarrier()
{
    return GetBarrier(D3D12_BARRIER_SYNC_RESOLVE, D3D12_BARRIER_ACCESS_RESOLVE_DEST, 
        D3D12_BARRIER_LAYOUT_RESOLVE_DEST);
}

D3D12_TEXTURE_BARRIER GDX12TextureResource::GetGenericReadBarrier()
{
    return GetBarrier(D3D12_BARRIER_SYNC_ALL, D3D12_BARRIER_ACCESS_COMMON,
       D3D12_BARRIER_LAYOUT_COMMON);
}
