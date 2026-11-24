#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"

// Resolves jittered color at its input resolution. History belongs to this pass/GPU
class GDX12TemporalAntiAliasingPass final : public GDX12RenderPass
{
public:
    GDX12TemporalAntiAliasingPass();
    void Describe(PassDescription& description) const override;
    void Initialize(PassInitContext& context) override;
    void OnResourcesRecreated(PassInitContext& context) override;
    std::vector<ExternalResourceUse> GetExternalResources(PassExecutionContext& context) override;
    void Execute(PassExecutionContext& context) override;

    // Discards accumulated color, for example after a camera cut.
    void ResetHistory();

private:
    // Allocates one previous-frame history and one write target, reusing their descriptors on resize.
    void CreateHistory(PassInitContext& context);

    std::unique_ptr<GDX12RootSignature> _root;
    ComPtr<ID3D12PipelineState> _pipeline;
    // RGB stores accumulated linear color; alpha stores the closest 3x3 surface's
    // normalized linear depth (zero where the entire neighborhood is sky).
    std::unique_ptr<GDX12Texture> _history;
    std::unique_ptr<GDX12Texture> _nextHistory;
    bool _historyValid = false;
    UINT _historyCameraIndex = UINT_MAX;
    Matrix _historyProjection = Matrix::Identity;
};
