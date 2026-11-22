#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"

// Renders all shadow projections every frame from the three shadow DrawStreams.
// FrameGraph owns both depth arrays and can transfer the inputs and outputs between GPUs.
// Private point/spot static and final caches survive slot changes. Only dirty lights are drawn;
// the complete final cache is copied into the current FrameGraph output for its consumers.
class GDX12ShadowMapPass final : public GDX12RenderPass
{
public:
    static constexpr const char* InDirectionalDrawStream = "InDirectionalDrawStream";
    static constexpr const char* InPointSpotStaticDrawStream = "InPointSpotStaticDrawStream";
    static constexpr const char* InPointSpotDynamicDrawStream = "InPointSpotDynamicDrawStream";
    static constexpr const char* OutDirectionalMaps = "OutDirectionalMaps";
    static constexpr const char* OutPointSpotMaps = "OutPointSpotMaps";

    GDX12ShadowMapPass();
    void Describe(PassDescription& description) const override;
    void ResolveResourceRequirements(PassResourceRequirementsContext& context) override;
    void Initialize(PassInitContext& context) override;
    void Execute(PassExecutionContext& context) override;
    // Rebuilds caches when the array size changes and refreshes all views after resource reconstruction.
    void OnResourcesRecreated(PassInitContext& context) override;
    // Retains private allocations through GPU completion and tracks their states on this GPU's DIRECT queue.
    std::vector<ExternalResourceUse> GetExternalResources(PassExecutionContext& context) override;

private:
    // Cache textures belong to this pass/GPU and survive frame-slot output reconstruction.
    // FrameGraph drains the queues before this callback can replace their allocations.
    void CreatePointSpotCache(PassInitContext& context);

    std::unique_ptr<GDX12RootSignature> _root;
    ComPtr<ID3D12CommandSignature> _signature;
    ComPtr<ID3D12PipelineState> _directionalPipeline;
    ComPtr<ID3D12PipelineState> _pointSpotPipeline;
    std::unique_ptr<GDX12Texture> _pointSpotStaticCache;
    std::unique_ptr<GDX12Texture> _pointSpotFinalCache;
    bool _resetCache = true;
};
