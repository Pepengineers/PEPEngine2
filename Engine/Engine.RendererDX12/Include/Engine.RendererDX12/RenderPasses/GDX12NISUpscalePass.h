#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include "Engine.RendererDX12/GDX12RootSignature.h"

class GDX12NISUpscalePass final : public GDX12RenderPass
{
public:
    explicit GDX12NISUpscalePass(float ratio = 1.5f);
    void Setup(GDX12DeviceResources* resources, RenderPipelineCommonData* common) override;
    void QueryRenderTargetResolution() override;
    void Initialize() override;
    void Resize() override;
    void Execute(GDX12CommandList* commands) override;

private:
    void CreateScaler();

    const float _ratio;
    UINT _blockWidth = 0;
    UINT _blockHeight = 0;
    std::unique_ptr<GDX12RootSignature> _root;
    ComPtr<ID3D12PipelineState> _pipeline;
    std::array<GDX12Texture, 2> _coefficients;
    GDX12Texture _output;
};
