#pragma once

#include "Engine.RendererDX12/D3DHelpers.h"

class GDX12Resource
{
public:
	GDX12Resource();
	~GDX12Resource() = default;

    CD3DX12_RESOURCE_BARRIER GetRenderTargetBarrier();
    CD3DX12_RESOURCE_BARRIER GetPixelShaderResourceBarrier();
    CD3DX12_RESOURCE_BARRIER GetNonPixelShaderResourceBarrier();
    CD3DX12_RESOURCE_BARRIER GetUnorderedAccessBarrier();
    CD3DX12_RESOURCE_BARRIER GetCopyDestBarrier();
    CD3DX12_RESOURCE_BARRIER GetCopySourceBarrier();
    CD3DX12_RESOURCE_BARRIER GetDepthWriteBarrier();
    CD3DX12_RESOURCE_BARRIER GetDepthReadBarrier();
    CD3DX12_RESOURCE_BARRIER GetResolveSourceBarrier();
    CD3DX12_RESOURCE_BARRIER GetResolveDestBarrier();
    CD3DX12_RESOURCE_BARRIER GetGenericReadBarrier();
    CD3DX12_RESOURCE_BARRIER GetCommonBarrier();
    CD3DX12_RESOURCE_BARRIER GetPresentBarrier();

    void SetCurrentState(D3D12_RESOURCE_STATES newState);
    D3D12_RESOURCE_STATES GetCurrentState();

	ComPtr<ID3D12Resource> D3DResource;

private:
	D3D12_RESOURCE_STATES _currentState;
};