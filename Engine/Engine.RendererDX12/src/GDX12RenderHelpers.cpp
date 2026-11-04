#include "Engine.RendererDX12/GDX12RenderHelpers.h"
#include "Engine.RendererDX12/GDX12DeviceResources.h"
#include "Engine.RendererDX12/GDX12Device.h"
#include "Engine.RendererDX12/GDX12ShaderCompiler.h"

namespace
{
    GDX12TextureDesc Texture2D(GDX12DeviceResources& resources, const UINT width, const UINT height, const DXGI_FORMAT format)
    {
        GDX12TextureDesc desc;
        desc.Width = width;
        desc.Height = height;
        desc.Format = desc.SRVDesc.Format = format;
        desc.SRV_UAV_Heap = resources.SRV_UAV_Heap.get();
        desc.SRVHeapIndex = desc.SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength);
        desc.SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        desc.SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        desc.SRVDesc.Texture2D.MipLevels = 1;
        return desc;
    }
}

void GDX12RenderHelpers::InitializeRenderTarget(GDX12Texture& texture, GDX12DeviceResources& resources,
    const UINT width, const UINT height, const DXGI_FORMAT format, const XMFLOAT4 clear)
{
    auto desc = Texture2D(resources, width, height, format);
    desc.ClearValue = clear;
    desc.CreateRTV = true;
    desc.RTVHeap = resources.RTVHeap.get();
    desc.RTVHeapIndex = desc.RTVHeap->GetAvailableIndex();
    desc.RTVDesc.Format = format;
    desc.RTVDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    texture.Initialize(desc);
}

void GDX12RenderHelpers::InitializeComputeTexture(GDX12Texture& texture, GDX12DeviceResources& resources,
    const UINT width, const UINT height, const DXGI_FORMAT format, const bool renderTarget)
{
    auto desc = Texture2D(resources, width, height, format);
    desc.CreateUAV = true;
    desc.UAVHeapIndex = desc.SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength);
    desc.UAVDesc.Format = format;
    desc.UAVDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    if (renderTarget)
    {
        desc.CreateRTV = true;
        desc.RTVHeap = resources.RTVHeap.get();
        desc.RTVHeapIndex = desc.RTVHeap->GetAvailableIndex();
        desc.RTVDesc.Format = format;
        desc.RTVDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    }
    texture.Initialize(desc);
}

ComPtr<ID3D12PipelineState> GDX12RenderHelpers::CreateComputePipeline(GDX12Device* device, GDX12RootSignature* root,
    const wchar_t* file, const char* entry, const D3D_SHADER_MACRO* defines)
{
    auto shader = GDX12ShaderCompiler::GetInstance().CompileShader(device, file, defines, entry, "cs");
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = root->GetRootSignature().Get();
    desc.CS = { shader->GetBufferPointer(), shader->GetBufferSize() };
    ComPtr<ID3D12PipelineState> result;
    ThrowIfFailed(device->GetDevice()->CreateComputePipelineState(&desc, IID_PPV_ARGS(&result)));
    return result;
}

ComPtr<ID3D12CommandSignature> GDX12RenderHelpers::CreateIndexedDrawSignature(GDX12Device* device, GDX12RootSignature* root)
{
    D3D12_INDIRECT_ARGUMENT_DESC arguments[2] = {};
    arguments[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
    arguments[0].Constant.Num32BitValuesToSet = 1;
    arguments[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
    D3D12_COMMAND_SIGNATURE_DESC desc = {};
    desc.ByteStride = sizeof(GDX12IndirectDrawArgs);
    desc.NumArgumentDescs = 2;
    desc.pArgumentDescs = arguments;
    ComPtr<ID3D12CommandSignature> result;
    ThrowIfFailed(device->GetDevice()->CreateCommandSignature(&desc, root->GetRootSignature().Get(), IID_PPV_ARGS(&result)));
    return result;
}
