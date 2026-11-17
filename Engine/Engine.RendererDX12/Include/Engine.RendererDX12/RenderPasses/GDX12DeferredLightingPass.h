#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"
#include "Engine.RendererDX12/GDX12LightGrid.h"
#include <stdexcept>
namespace Meta { class SceneLighting; }

class GDX12DeferredLightingPass final : public GDX12RenderPass
{
public:

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
            _inputs[0]->GetTexture()->GetWidth(), _inputs[0]->GetTexture()->GetHeight(), DXGI_FORMAT_R16G16B16A16_FLOAT, true);
        GDX12RootSignatureDesc root;
        root.NumSingleCBVSlots = 5;
        root.NumSingleSRVSlots = 18;
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
        D3D12_FEATURE_DATA_D3D12_OPTIONS1 options = {};
        const bool waveOps = SUCCEEDED(_resources->Device->GetDevice()->CheckFeatureSupport(
            D3D12_FEATURE_D3D12_OPTIONS1, &options, sizeof(options))) && options.WaveOps &&
            _resources->Device->GetDeviceFeatures().Features.HighestShaderModel() >= D3D_SHADER_MODEL_6_0;
        const D3D_SHADER_MACRO defines[] = { { "DEFERRED_WAVE_OPS", waveOps ? "1" : "0" },
            { "DEFERRED_OPAQUE_ONLY", _opaqueOnly ? "1" : "0" }, { nullptr, nullptr } };
        _pipeline = GDX12RenderHelpers::CreateComputePipeline(_resources->Device, _root.get(),
            SHADERS_FOLDER "DeferredLighting.hlsl", "CS", defines);
        _grids.resize(_resources->FrameConstants.size());
        for (auto& grid : _grids)
        {
            grid.Upload = std::make_unique<GDX12UploadBuffer<UINT>>(_resources->Device, 1, EBufferType::Upload, false);
            grid.Masks = std::make_unique<GDX12UploadBuffer<UINT>>(_resources->Device, 1, EBufferType::Default, false);
            grid.Masks->CreateSRV(_resources->SRV_UAV_Heap.get(), _resources->SRV_UAV_Heap->GetAvailableIndex(ConstantsResources));
            grid.Constants = std::make_unique<GDX12UploadBuffer<GDX12LightGridConstants>>(_resources->Device, 1);
        }
    }
    void Resize() override { _color->Resize(_inputs[0]->GetTexture()->GetWidth(), _inputs[0]->GetTexture()->GetHeight()); }
    void ClearDenendencies() override
    {
        GDX12RenderPass::ClearDenendencies();
        _color.reset(); _root.reset(); _pipeline.Reset(); _grids.clear();
    }
private:
    friend class Meta::SceneLighting;
    bool _opaqueOnly = false;
    struct FrameGrid
    {
        std::unique_ptr<GDX12UploadBuffer<UINT>> Upload, Masks;
        std::unique_ptr<GDX12UploadBuffer<GDX12LightGridConstants>> Constants;
    };
    GDX12LightGrid _grid;
    GDX12LightGridBuildSystem _gridSystem;
    std::vector<FrameGrid> _grids;
    std::unique_ptr<GDX12Texture> _color;
    std::unique_ptr<GDX12RootSignature> _root;
    ComPtr<ID3D12PipelineState> _pipeline;
};
