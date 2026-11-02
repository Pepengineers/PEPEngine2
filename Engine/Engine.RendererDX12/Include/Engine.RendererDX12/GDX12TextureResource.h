#pragma once

#include "Engine.RendererDX12\D3DHelpers.h"
#include "Engine.RendererDX12/GDX12Resource.h"

class GDX12TextureResource : public GDX12Resource
{
public:
    GDX12TextureResource(ComPtr<ID3D12Resource> resource);
    GDX12TextureResource();
    ~GDX12TextureResource();

    // Enhanced texture barrier transition getters
    D3D12_TEXTURE_BARRIER GetRenderTargetEnhBarrier();
    D3D12_TEXTURE_BARRIER GetPixelShaderResourceEnhBarrier();
    D3D12_TEXTURE_BARRIER GetNonPixelShaderResourceEnhBarrier();
    D3D12_TEXTURE_BARRIER GetUnorderedAccessEnhBarrier();
    D3D12_TEXTURE_BARRIER GetCopyDestEnhBarrier();
    D3D12_TEXTURE_BARRIER GetCopySourceEnhBarrier();
    D3D12_TEXTURE_BARRIER GetDepthWriteEnhBarrier();
    D3D12_TEXTURE_BARRIER GetDepthReadEnhBarrier();
    D3D12_TEXTURE_BARRIER GetResolveSourceEnhBarrier();
    D3D12_TEXTURE_BARRIER GetResolveDestEnhBarrier();
    D3D12_TEXTURE_BARRIER GetGenericReadEnhBarrier();
    D3D12_TEXTURE_BARRIER GetCommonEnhBarrier();
    D3D12_TEXTURE_BARRIER GetPresentEnhBarrier();

    D3D12_TEXTURE_BARRIER GetBarrier(D3D12_BARRIER_SYNC syncAfter, 
        D3D12_BARRIER_ACCESS accessAfter, D3D12_BARRIER_LAYOUT layoutAfter);

    void SetCurrentState(D3D12_BARRIER_SYNC sync, D3D12_BARRIER_ACCESS access, D3D12_BARRIER_LAYOUT layout);
    void ResetState() override;
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