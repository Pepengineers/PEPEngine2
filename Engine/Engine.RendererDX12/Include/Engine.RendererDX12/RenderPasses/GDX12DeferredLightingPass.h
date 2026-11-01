#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"

class GDX12DeferredLightingPass final : public GDX12RenderPass
{
public:
    GDX12DeferredLightingPass()
    {
        _flags = RENDER_PASS_FLAG_USE_CAMERAS;
        _numInputs = 11;
        _numOutputs = 1;
        _color = std::make_unique<GDX12Texture>();
        _outputs.push_back(_color.get());
    }
    void Initialize() override
    {
        GDX12TextureDesc texture;
        texture.Width = _inputs[0]->GetTexture()->GetWidth();
        texture.Height = _inputs[0]->GetTexture()->GetHeight();
        texture.Format = texture.SRVDesc.Format = texture.UAVDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        texture.CreateUAV = true;
        texture.SRV_UAV_Heap = _resources->SRV_UAV_Heap.get();
        texture.SRVHeapIndex = texture.SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength);
        texture.UAVHeapIndex = texture.SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength);
        texture.SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        texture.SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        texture.SRVDesc.Texture2D.MipLevels = 1;
        texture.UAVDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        _color->Initialize(texture);
        GDX12RootSignatureDesc root;
        root.NumSingleCBVSlots = 3;
        root.NumSingleSRVSlots = 12;
        root.NumSingleUAVSlots = 1;
        root.StaticSamplers = GetStaticSamplers();
        CD3DX12_STATIC_SAMPLER_DESC sampler(6, D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT);
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
        root.StaticSamplers.push_back(sampler);
        _root = std::make_unique<GDX12RootSignature>(_resources->Device, root);
        auto shader = GDX12ShaderCompiler::GetInstance().CompileShader(_resources->Device,
            SHADERS_FOLDER "DeferredLighting.hlsl", nullptr, "CS", "cs");
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = _root->GetRootSignature().Get();
        desc.CS = { shader->GetBufferPointer(), shader->GetBufferSize() };
        ThrowIfFailed(_resources->Device->GetDevice()->CreateComputePipelineState(&desc, IID_PPV_ARGS(&_pipeline)));
    }
    void Execute(GDX12CommandList* commands) override
    {
        commands->BeginPixEvent("Tiled deferred lighting", Colors::Gold);
        auto& frame = _resources->FrameConstants[_resources->CurrFrameConstantsIndex];
        commands->SetComputeRootSignature(_root.get());
        commands->SetPipelineState(_pipeline.Get());
        commands->SetDescriptorHeaps({ _resources->SRV_UAV_Heap.get() });
        commands->SetComputeRootConstantBufferView(0, frame->MainCB->GetElementAddress(0));
        commands->SetComputeRootConstantBufferView(1, frame->CameraCB->GetElementAddress(_commonData->ActiveCameraCBufferIndex));
        commands->SetComputeRootConstantBufferView(2, frame->ShadowCB->GetElementAddress(0));
        for (UINT i = 0; i < _numInputs; ++i)
        {
            auto* texture = _inputs[i]->GetTexture();
            commands->ResourceBarrier({ texture->GetResource()->GetSRVBarrier() });
            commands->SetComputeSRV(i, texture->GetSRV()->GPUHandle);
        }
        commands->SetComputeSRV(11, frame->LocalLightCache->GetSRV()->GPUHandle);
        commands->SetComputeUAV(0, _color->GetUAV()->GPUHandle);
        commands->ResourceBarrier({ _color->GetResource()->GetUnorderedAccessBarrier() });
        commands->Dispatch((_color->GetWidth() + 15) / 16, (_color->GetHeight() + 15) / 16, 1);
        commands->ResourceBarrier({ _color->GetResource()->GetSRVBarrier() });
        commands->EndPixEvent();
    }
    void Resize() override { _color->Resize(_inputs[0]->GetTexture()->GetWidth(), _inputs[0]->GetTexture()->GetHeight()); }
    void ClearDenendencies() override
    {
        GDX12RenderPass::ClearDenendencies();
        _color.reset(); _root.reset(); _pipeline.Reset();
    }
private:
    std::unique_ptr<GDX12Texture> _color;
    std::unique_ptr<GDX12RootSignature> _root;
    ComPtr<ID3D12PipelineState> _pipeline;
};
