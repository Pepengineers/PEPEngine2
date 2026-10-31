#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include <array>

class GDX12EnvironmentIBLPass final : public GDX12RenderPass
{
public:
    GDX12EnvironmentIBLPass()
    {
        _numOutputs = 3;
        for (auto& texture : _textures)
        {
            texture = std::make_unique<GDX12Texture>();
            _outputs.push_back(texture.get());
        }
    }

    void Initialize() override
    {
        CreateTexture(*_textures[0], 32, 1, true);
        CreateTexture(*_textures[1], 128, 8, true);
        CreateTexture(*_textures[2], 128, 1, false);
        GDX12RootSignatureDesc root;
        root.Constants.push_back(2);
        root.NumSingleUAVSlots = 2;
        _root = std::make_unique<GDX12RootSignature>(_resources->Device, root);
        const char* entries[] = { "IrradianceCS", "PrefilterCS", "IntegrateBRDFCS" };
        for (UINT i = 0; i < 3; ++i)
        {
            auto shader = GDX12ShaderCompiler::GetInstance().CompileShader(_resources->Device,
                SHADERS_FOLDER "EnvironmentIBL.hlsl", nullptr, entries[i], "cs");
            D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
            desc.pRootSignature = _root->GetRootSignature().Get();
            desc.CS = { shader->GetBufferPointer(), shader->GetBufferSize() };
            ThrowIfFailed(_resources->Device->GetDevice()->CreateComputePipelineState(&desc, IID_PPV_ARGS(&_pipelines[i])));
        }
        for (UINT mip = 0; mip < 8; ++mip)
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC view = {};
            view.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
            view.Texture2DArray.ArraySize = 6;
            view.Texture2DArray.MipSlice = mip;
            _prefilterUAVs[mip].InitAsUAV(_textures[1]->GetResource()->D3DResource.Get(), nullptr, &view,
                _resources->SRV_UAV_Heap.get(), _resources->SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength));
        }
    }

    void Execute(GDX12CommandList* commands) override
    {
        if (_ready) return;
        commands->BeginPixEvent("Prepare environment IBL", Colors::CornflowerBlue);
        commands->SetDescriptorHeaps({ _resources->SRV_UAV_Heap.get() });
        commands->SetComputeRootSignature(_root.get());
        for (auto& texture : _textures)
            commands->ResourceBarrier({ texture->GetResource()->GetUnorderedAccessBarrier() });
        Dispatch(commands, 0, 32, 0.0f, _textures[0]->GetUAV()->GPUHandle, 0, 6);
        for (UINT mip = 0; mip < 8; ++mip)
            Dispatch(commands, 1, 128u >> mip, mip / 7.0f, _prefilterUAVs[mip].GPUHandle, 0, 6);
        Dispatch(commands, 2, 128, 0.0f, _textures[2]->GetUAV()->GPUHandle, 1, 1);
        for (auto& texture : _textures)
            commands->ResourceBarrier({ texture->GetResource()->GetSRVBarrier() });
        commands->EndPixEvent();
        _ready = true;
    }

    void ClearDenendencies() override
    {
        GDX12RenderPass::ClearDenendencies();
        for (auto& texture : _textures) texture.reset();
        for (auto& pipeline : _pipelines) pipeline.Reset();
        _root.reset();
    }

private:
    void CreateTexture(GDX12Texture& texture, UINT size, UINT mips, bool cube)
    {
        const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        const auto resourceDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R16G16B16A16_FLOAT,
            size, size, cube ? 6 : 1, static_cast<UINT16>(mips), 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        GDX12TextureDesc desc;
        ThrowIfFailed(_resources->Device->GetDevice()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
            &resourceDesc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&desc.ExternalResource)));
        desc.Format = desc.SRVDesc.Format = desc.UAVDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.Width = desc.Height = size;
        desc.SRV_UAV_Heap = _resources->SRV_UAV_Heap.get();
        desc.SRVHeapIndex = desc.SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength);
        desc.UAVHeapIndex = desc.SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength);
        desc.SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        desc.SRVDesc.ViewDimension = cube ? D3D12_SRV_DIMENSION_TEXTURECUBE : D3D12_SRV_DIMENSION_TEXTURE2D;
        if (cube) desc.SRVDesc.TextureCube.MipLevels = mips;
        else desc.SRVDesc.Texture2D.MipLevels = mips;
        desc.CreateUAV = true;
        desc.UAVDesc.ViewDimension = cube ? D3D12_UAV_DIMENSION_TEXTURE2DARRAY : D3D12_UAV_DIMENSION_TEXTURE2D;
        if (cube) desc.UAVDesc.Texture2DArray.ArraySize = 6;
        texture.Initialize(desc);
    }

    void Dispatch(GDX12CommandList* commands, UINT pipeline, UINT size, float roughness,
        D3D12_GPU_DESCRIPTOR_HANDLE uav, UINT slot, UINT faces)
    {
        struct { UINT Size; float Roughness; } parameters = { size, roughness };
        commands->SetPipelineState(_pipelines[pipeline]);
        commands->GetCommandList()->SetComputeRoot32BitConstants(0, 2, &parameters, 0);
        commands->SetComputeUAV(slot, uav);
        commands->Dispatch((size + 7) / 8, (size + 7) / 8, faces);
    }

    std::array<std::unique_ptr<GDX12Texture>, 3> _textures;
    std::array<GDX12Descriptor, 8> _prefilterUAVs;
    std::array<ComPtr<ID3D12PipelineState>, 3> _pipelines;
    std::unique_ptr<GDX12RootSignature> _root;
    bool _ready = false;
};
