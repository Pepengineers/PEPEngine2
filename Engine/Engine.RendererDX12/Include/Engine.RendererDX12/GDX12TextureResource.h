#pragma once

#include "Engine.RendererDX12\D3DHelpers.h"

class GDX12TextureResource
{
public:
    GDX12TextureResource(ComPtr<ID3D12Resource> resource);
    ~GDX12TextureResource();

    ComPtr<ID3D12Resource> D3DResource;

    // Enhanced texture barrier transition getters
    D3D12_TEXTURE_BARRIER GetRenderTargetBarrier();
    D3D12_TEXTURE_BARRIER GetPixelShaderResourceBarrier();
    D3D12_TEXTURE_BARRIER GetNonPixelShaderResourceBarrier();
    D3D12_TEXTURE_BARRIER GetUnorderedAccessBarrier();
    D3D12_TEXTURE_BARRIER GetCopyDestBarrier();
    D3D12_TEXTURE_BARRIER GetCopySourceBarrier();
    D3D12_TEXTURE_BARRIER GetDepthWriteBarrier();
    D3D12_TEXTURE_BARRIER GetDepthReadBarrier();
    D3D12_TEXTURE_BARRIER GetResolveSourceBarrier();
    D3D12_TEXTURE_BARRIER GetResolveDestBarrier();
    D3D12_TEXTURE_BARRIER GetGenericReadBarrier();
    D3D12_TEXTURE_BARRIER GetCommonBarrier();
    D3D12_TEXTURE_BARRIER GetPresentBarrier();

    D3D12_TEXTURE_BARRIER GetBarrier(D3D12_BARRIER_SYNC syncAfter, 
        D3D12_BARRIER_ACCESS accessAfter, D3D12_BARRIER_LAYOUT layoutAfter);

    void SetCurrentState(D3D12_BARRIER_SYNC sync, D3D12_BARRIER_ACCESS access, D3D12_BARRIER_LAYOUT layout);

    D3D12_BARRIER_SYNC GetCurrentSync();
    D3D12_BARRIER_ACCESS GetCurrentAccess();
    D3D12_BARRIER_LAYOUT GetCurrentLayout();

private:
    D3D12_TEXTURE_BARRIER CreateBarrier(
        D3D12_BARRIER_SYNC syncAfter,
        D3D12_BARRIER_ACCESS accessAfter,
        D3D12_BARRIER_LAYOUT layoutAfter);

    D3D12_RESOURCE_DESC _desc;
    D3D12_BARRIER_SYNC _currentSync;
    D3D12_BARRIER_ACCESS _currentAccess;
    D3D12_BARRIER_LAYOUT _currentLayout;
};