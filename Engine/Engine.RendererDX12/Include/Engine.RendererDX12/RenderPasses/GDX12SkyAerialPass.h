#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
class GDX12EnvironmentIBLPass;
class GDX12SkyAerialPass final : public GDX12RenderPass
{
public:
    explicit GDX12SkyAerialPass(GDX12EnvironmentIBLPass* environment);
    ~GDX12SkyAerialPass() override;
    void Initialize() override;
    void Resize() override { Initialize(); }
    void Execute(GDX12CommandList*) override;
    void ClearDenendencies() override;
private:
    GDX12EnvironmentIBLPass* _environment;
    GDX12Texture _color;
};
