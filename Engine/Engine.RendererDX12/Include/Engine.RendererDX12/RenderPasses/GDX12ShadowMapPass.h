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
    static void ValidateShadowMapResolutions(UINT directionalResolution, UINT pointSpotResolution);

    GDX12ShadowMapPass();
    void Describe(PassDescription& description) const override;
    void ResolveResourceRequirements(PassResourceRequirementsContext& context) override;
    void Initialize(PassInitContext& context) override;
    void Execute(PassExecutionContext& context) override;
    void OnResourcesRecreated(PassInitContext& context) override;
    std::vector<ExternalResourceUse> GetExternalResources(PassExecutionContext& context) override;

private:
    // Static depth belongs to this pass/GPU. FrameGraph drains the queues before reconstruction
    // can replace its allocation; final depth is owned by FrameGraph and reused across slots.
    void CreatePointSpotCache(PassInitContext& context);

    // Hi-Z and compacted commands have one consumer and live on this pass's GPU.
    // Recreated after the graph drains its queues when resolution or capacity changes.
    void CreateDirectionalOcclusionResources(PassInitContext& context);
    // Renders opaque occluders, builds a conservative max-depth pyramid for this frame,
    // then draws only the remaining candidates not completely hidden from the light.
    void RenderDirectionalShadows(PassExecutionContext& context, GDX12Texture* directionalShadowMaps, const DrawStreamBinding& directionalCandidateDraws);

    std::unique_ptr<GDX12RootSignature> _shadowRootSignature;
    ComPtr<ID3D12CommandSignature> _shadowDrawCommandSignature;
    ComPtr<ID3D12PipelineState> _directionalShadowPipeline;
    ComPtr<ID3D12PipelineState> _pointSpotShadowPipeline;
    std::unique_ptr<GDX12Texture> _pointSpotStaticDepthCache;
    XMFLOAT4 _cascadeMinOccluderSizeTexels = {};
    std::unique_ptr<GDX12RootSignature> _directionalOcclusionRootSignature;
    ComPtr<ID3D12PipelineState> _clearDirectionalDrawCountsPipeline;
    ComPtr<ID3D12PipelineState> _selectDirectionalOccludersPipeline;
    ComPtr<ID3D12PipelineState> _reduceDirectionalHiZDepthPipeline;
    ComPtr<ID3D12PipelineState> _directionalOcclusionCullingPipeline;
    std::unique_ptr<GDX12UploadBuffer<GDX12ShadowDrawArgs>> _directionalOccluderCommands;
    std::unique_ptr<GDX12UploadBuffer<GDX12ShadowDrawArgs>> _directionalVisibleCommands;
    // Two counts: occluder commands at byte offset 0, remaining visible commands at 4.
    std::unique_ptr<GDX12UploadBuffer<UINT>> _directionalDrawCounts;
    std::unique_ptr<GDX12Texture> _directionalHiZDepthTexture;
    // Reduction reads mips [0, count-2]; mip 0's UAV is already owned by the texture.
    std::vector<std::unique_ptr<GDX12Descriptor>> _directionalHiZMipSRVs;
    std::vector<std::unique_ptr<GDX12Descriptor>> _directionalHiZMipUAVs;
    bool _resetShadowCache = true;
};
