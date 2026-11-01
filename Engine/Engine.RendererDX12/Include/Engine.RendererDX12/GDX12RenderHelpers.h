#pragma once

#include "Engine.RendererDX12/D3DHelpers.h"

class GDX12Device;
class GDX12DeviceResources;
class GDX12RootSignature;
class GDX12Texture;

namespace GDX12RenderHelpers
{
    void InitializeRenderTarget(GDX12Texture& texture, GDX12DeviceResources& resources,
        UINT width, UINT height, DXGI_FORMAT format, XMFLOAT4 clear = { 0, 0, 0, 1 });
    void InitializeComputeTexture(GDX12Texture& texture, GDX12DeviceResources& resources,
        UINT width, UINT height, DXGI_FORMAT format);
    ComPtr<ID3D12PipelineState> CreateComputePipeline(GDX12Device* device, GDX12RootSignature* root,
        const wchar_t* file, const char* entry, const D3D_SHADER_MACRO* defines = nullptr);
    ComPtr<ID3D12CommandSignature> CreateIndexedDrawSignature(GDX12Device* device, GDX12RootSignature* root);
}
