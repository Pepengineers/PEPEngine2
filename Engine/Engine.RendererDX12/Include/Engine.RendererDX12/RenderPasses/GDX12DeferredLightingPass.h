#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"

class GDX12DeferredLightingPass final : public GDX12RenderPass
{
public:
    void RecordRenderGraph(GDX12RenderGraphBuilder& graph) override
    {
        graph.SetName("Deferred lighting");
        graph.Read(SharedResource::Scene);
        graph.ReadInputs(_inputs, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        graph.Read(SharedResource::Environment);
        graph.Read(SharedResource::ReflectionProbes);
        graph.Write(_outputs[0], "Deferred HDR", Lifetime::Transient, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    GDX12DeferredLightingPass()
    {
        _flags = RENDER_PASS_FLAG_USE_CAMERAS;
        _numInputs = 12;
        _numOutputs = 1;
        _color = std::make_unique<GDX12Texture>();
        _outputs.push_back(_color.get());
    }
    void Initialize() override
    {
        GDX12RenderHelpers::InitializeComputeTexture(*_color, *_resources,
            _inputs[0]->GetTexture()->GetWidth(), _inputs[0]->GetTexture()->GetHeight(), DXGI_FORMAT_R16G16B16A16_FLOAT);
        GDX12RootSignatureDesc root;
        root.NumSingleCBVSlots = 4;
        root.NumSingleSRVSlots = 17;
        root.NumSingleUAVSlots = 1;
        root.StaticSamplers = GetStaticSamplers();
        root.StaticSamplers.emplace_back(7, D3D12_FILTER_MIN_MAG_MIP_LINEAR,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
        CD3DX12_STATIC_SAMPLER_DESC sampler(6, D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT);
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
        root.StaticSamplers.push_back(sampler);
        _root = std::make_unique<GDX12RootSignature>(_resources->Device, root);
        _pipeline = GDX12RenderHelpers::CreateComputePipeline(_resources->Device, _root.get(),
            SHADERS_FOLDER "DeferredLighting.hlsl", "CS");
    }
    void Execute(GDX12CommandList* commands) override
    {
        GDX12_SCOPED_EVENT(commands, "Tiled deferred lighting", Colors::Gold);
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
            commands->SetComputeSRV(i < 11 ? i : 14, texture->GetSRV()->GPUHandle);
        }
        commands->SetComputeRootConstantBufferView(3, frame->SkyCB->GetElementAddress(0));
        commands->SetComputeSRV(15, _resources->SkyTransmittanceSRV.GPUHandle);
        commands->SetComputeSRV(16, _resources->SkyViewSRV.GPUHandle);
        commands->SetComputeSRV(11, frame->LocalLightCache->GetSRV()->GPUHandle);
        commands->SetComputeSRV(12, _resources->ReflectionRadianceSRV.GPUHandle);
        commands->SetComputeSRV(13, frame->ReflectionProbes->GetSRV()->GPUHandle);
        commands->SetComputeUAV(0, _color->GetUAV()->GPUHandle);
        commands->Dispatch((_color->GetWidth() + 15) / 16, (_color->GetHeight() + 15) / 16, 1);
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
