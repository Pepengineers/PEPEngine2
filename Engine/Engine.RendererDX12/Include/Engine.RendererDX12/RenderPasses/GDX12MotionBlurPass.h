#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"

// Inputs: unjittered, matching-grid linear HDR RGBA16F, reverse-Z depth and previous-minus-current NDC motion.
class GDX12MotionBlurPass final : public GDX12RenderPass
{
public:
    GDX12MotionBlurPass();
    ~GDX12MotionBlurPass() override;
    void Initialize() override;
    void PrepareFrame() override;
    void Execute(GDX12CommandList* commands) override;
    void Resize() override;
    void ClearDenendencies() override;
    bool IsEnabled() const;
private:
    struct Data;
    std::unique_ptr<Data> _data;
};
