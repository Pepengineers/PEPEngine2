#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include "Engine.RendererDX12/GDX12Sky.h"
class GDX12EnvironmentIBLPass final : public GDX12RenderPass
{
public:
    GDX12EnvironmentIBLPass();
    ~GDX12EnvironmentIBLPass() override;
    void Initialize() override;
    void PrepareFrame() override;
    void Execute(GDX12CommandList*) override;
    void ClearDenendencies() override;
    void Submit(GDX12SkyDesc sky);
    UINT GetSource() const;
    GDX12Texture* GetTransmittanceLUT();
    GDX12Texture* GetMultiScatteringLUT();
    GDX12Texture* GetSkyViewLUT();
    GDX12Texture* GetAerialTransmittance();
    void ApplyAerial(GDX12CommandList*,GDX12Texture& color,GDX12Texture& depth,GDX12Texture& output);
private:
    struct Data;
    std::unique_ptr<Data> _data;
};
