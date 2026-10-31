#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"

// Input: linear HDR RGBA16F. Output: Reinhard + sRGB RGBA8 UNORM, ready for the swapchain copy.
class GDX12ToneMappingPass final : public GDX12RenderPass
{
public:
    GDX12ToneMappingPass();
    ~GDX12ToneMappingPass() override;
    void Initialize() override;
    void Execute(GDX12CommandList* commands) override;
    void Resize() override;
    void ClearDenendencies() override;
private:
    struct Data;
    std::unique_ptr<Data> _data;
};
