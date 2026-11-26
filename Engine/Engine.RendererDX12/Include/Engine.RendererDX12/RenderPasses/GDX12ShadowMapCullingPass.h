#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"

class GDX12ShadowMapCullingPass final : public GDX12RenderPass
{
public:
    static constexpr const char* OutDirectionalDrawStream = "OutDirectionalDrawStream";
    static constexpr const char* OutPointSpotStaticDrawStream = "OutPointSpotStaticDrawStream";
    static constexpr const char* OutPointSpotDynamicDrawStream = "OutPointSpotDynamicDrawStream";

    GDX12ShadowMapCullingPass();
    void Describe(PassDescription& description) const override;
    void ResolveResourceRequirements(PassResourceRequirementsContext& context) override;
    void Initialize(PassInitContext& context) override;
    void Execute(PassExecutionContext& context) override;
    // Generate static and dynamic draws once after shadow resources are recreated.
    void OnResourcesRecreated(PassInitContext& context) override;

private:
    std::unique_ptr<GDX12RootSignature> _root;
    ComPtr<ID3D12PipelineState> _cullingPipeline;
    ComPtr<ID3D12PipelineState> _clearPipeline;
    bool _resetCache = true;
};
