#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"

struct FrameGraphPhysicalResource;

// Camera culling combines frustum rejection and current-frame Hi-Z occlusion.
// Frustum candidates are tested against this frame's opaque occluders. Occluder depth
// and Hi-Z stay private on the pass, it outputs the final DrawStreams only.
class GDX12OcclusionCullingPass final : public GDX12RenderPass
{
public:
    static constexpr const char* OutOpaqueDrawStream = "OutOpaqueDrawStream";
    static constexpr const char* OutTransparentDrawStream = "OutTransparentDrawStream";

    GDX12OcclusionCullingPass();
    ~GDX12OcclusionCullingPass() override;
    void Describe(PassDescription& description) const override;
    void ResolveResourceRequirements(PassResourceRequirementsContext& context) override;
    void Initialize(PassInitContext& context) override;
    void Execute(PassExecutionContext& context) override;
    void OnResourcesRecreated(PassInitContext& context) override;
    std::vector<ExternalResourceUse> GetExternalResources(PassExecutionContext& context) override;

private:
    void CullFrustum(PassExecutionContext& context, const DrawStreamBinding& opaqueCandidateDraws, const DrawStreamBinding& transparentCandidateDraws);

    // Rebuilds local streams and textures when scene capacity or render resolution changes.
    void CreateWorkingResources(PassInitContext& context);

    std::unique_ptr<GDX12RootSignature> _frustumCullingRootSignature;
    std::unique_ptr<GDX12RootSignature> _clearCandidateCountsRootSignature;
    ComPtr<ID3D12PipelineState> _frustumCullingPipeline;
    ComPtr<ID3D12PipelineState> _clearCandidateCountsPipeline;
    float _minOccluderSizePixels = 128;
    std::unique_ptr<GDX12RootSignature> _occlusionCullingRootSignature;
    std::unique_ptr<GDX12RootSignature> _occluderDepthRootSignature;
    ComPtr<ID3D12CommandSignature> _occluderDrawCommandSignature;
    ComPtr<ID3D12PipelineState> _clearDrawCountsPipeline;
    ComPtr<ID3D12PipelineState> _selectOccludersPipeline;
    ComPtr<ID3D12PipelineState> _reduceHiZDepthPipeline;
    ComPtr<ID3D12PipelineState> _occlusionCullingPipeline;
    ComPtr<ID3D12PipelineState> _occluderDepthPipeline;
    std::unique_ptr<FrameGraphPhysicalResource> _opaqueCandidateDrawStream;
    std::unique_ptr<FrameGraphPhysicalResource> _transparentCandidateDrawStream;
    std::unique_ptr<FrameGraphPhysicalResource> _occluderDrawStream;
    std::unique_ptr<GDX12Texture> _occluderDepthTexture;
    std::unique_ptr<GDX12Texture> _hiZDepthTexture;
    std::vector<std::unique_ptr<GDX12Descriptor>> _hiZMipSRVs;
    std::vector<std::unique_ptr<GDX12Descriptor>> _hiZMipUAVs;
};
