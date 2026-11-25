#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include <GFSDK_SSAO.h>

// Official NVIDIA HBAO+ 4.0 DX12 integration. Radius/MaxDepth use centimetres,
// Sharpness uses inverse centimetres, Bias is dimensionless, and Intensity
// is the AO visibility exponent (0 disables AO, otherwise [1,4]).
class GDX12HBAOPlusPass final : public GDX12RenderPass
{
public:
    GDX12HBAOPlusPass();
    void Describe(PassDescription& description) const override;
    void Initialize(PassInitContext& context) override;
    void Execute(PassExecutionContext& context) override;
    void OnResourcesRecreated(PassInitContext& context) override;

private:
    // Each FrameGraph resource slot owns an SDK context: the SDK overwrites its
    // upload constant buffer during RenderAO, so in-flight frames cannot share it.
    struct ContextResources
    {
        ~ContextResources();
        GFSDK_SSAO_Context_D3D12* Context = nullptr;
        std::unique_ptr<GDX12DescriptorHeap> ShaderHeap;
        std::unique_ptr<GDX12DescriptorHeap> RTVHeap;
        ComPtr<ID3D12Resource> Depth;
        ComPtr<ID3D12Resource> Normals;
    };

    // Creates the SDK context and its private heaps on the pass's GPU.
    // FrameGraph waits for this resource slot before its upload data is reused.
    ContextResources& GetContext(PassExecutionContext& context);

    // Updates SRVs only when their input resource changes. Retained references
    // keep cached resource addresses valid until their descriptors are replaced.
    static void UpdateInputViews(PassExecutionContext& context, ContextResources& slot);
    static void Check(GFSDK_SSAO_Status result, const char* operation);

    GFSDK_SSAO_Parameters _parameters;
    std::vector<std::unique_ptr<ContextResources>> _contexts;
};
