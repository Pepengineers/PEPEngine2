#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"

// Renders shadow projections from the three shadow DrawStreams.
// FrameGraph owns both depth arrays and can transfer the inputs and outputs between GPUs.
// Static depth and FrameGraph output arrays are reused across frame slots.
// Changed lights are restored from static depth and receive dynamic draws once.
class GDX12ShadowMapPass final : public GDX12RenderPass
{
public:
    static constexpr const char* InDirectionalDrawStream = "InDirectionalDrawStream";
    static constexpr const char* InPointSpotStaticDrawStream = "InPointSpotStaticDrawStream";
    static constexpr const char* InPointSpotDynamicDrawStream = "InPointSpotDynamicDrawStream";
    static constexpr const char* OutDirectionalMaps = "OutDirectionalMaps";
    static constexpr const char* OutPointSpotMaps = "OutPointSpotMaps";

    // Checks texture limits and the minimum size required for directional texel snapping.
    // RenderModule validates the configuration before changing the shared CPU settings.
    static void ValidateShadowMapResolutions(UINT directional, UINT pointSpot);

    GDX12ShadowMapPass();
    void Describe(PassDescription& description) const override;
    void ResolveResourceRequirements(PassResourceRequirementsContext& context) override;
    void Initialize(PassInitContext& context) override;
    void Execute(PassExecutionContext& context) override;
    // Rebuilds static depth when needed and initializes the new shared output arrays.
    void OnResourcesRecreated(PassInitContext& context) override;
    // Retains static depth through GPU completion and tracks its state on this GPU's DIRECT queue.
    std::vector<ExternalResourceUse> GetExternalResources(PassExecutionContext& context) override;

private:
    // Static depth belongs to this pass/GPU. FrameGraph drains the queues before reconstruction
    // can replace its allocation; final depth is owned by FrameGraph and reused across slots.
    void CreatePointSpotCache(PassInitContext& context);

    std::unique_ptr<GDX12RootSignature> _root;
    ComPtr<ID3D12CommandSignature> _signature;
    ComPtr<ID3D12PipelineState> _directionalPipeline;
    ComPtr<ID3D12PipelineState> _pointSpotPipeline;
    std::unique_ptr<GDX12Texture> _pointSpotStaticCache;
    bool _resetCache = true;
};
