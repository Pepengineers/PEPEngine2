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
        // Non-owning device resources; this context owns only the reserved ranges.
        GDX12DeviceResources* DeviceResources = nullptr;
        UINT ShaderDescriptorsStart = UINT_MAX;
        UINT RTVDescriptorsStart = UINT_MAX;
    };

    // Creates the SDK context using reserved ranges in the pass GPU's shared heaps.
    // FrameGraph waits for this resource slot before its upload data is reused.
    ContextResources& GetContext(PassExecutionContext& context);

    static void Check(GFSDK_SSAO_Status result, const char* operation);

    GFSDK_SSAO_Parameters _parameters;
    std::vector<std::unique_ptr<ContextResources>> _contexts;
};
