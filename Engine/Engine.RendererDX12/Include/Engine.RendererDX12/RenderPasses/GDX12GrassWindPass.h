#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include "Engine.RendererDX12/GDX12GrassWind.h"

class GDX12GrassWindPass final : public GDX12RenderPass
{
public:
    GDX12GrassWindPass();
    ~GDX12GrassWindPass() override;
    void Initialize() override;
    void Submit(GDX12GrassWindDesc submission);
    void PrepareFrame() override;
    void Execute(GDX12CommandList* commands) override;
    void ClearDenendencies() override;
private:
    struct Data;
    std::unique_ptr<Data> _data;
};
