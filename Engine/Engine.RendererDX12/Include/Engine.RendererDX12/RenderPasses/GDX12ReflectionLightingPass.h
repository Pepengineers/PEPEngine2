#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include "Engine.RendererDX12/GDX12ReflectionProbes.h"
class GDX12ReflectionProbePass;

class GDX12ReflectionLightingPass final : public GDX12RenderPass
{
public:
    explicit GDX12ReflectionLightingPass(GDX12ReflectionProbePass* source = nullptr);
    ~GDX12ReflectionLightingPass() override;
    void Setup(GDX12DeviceResources*, GDX12DeviceResources*, RenderPipelineCommonData*) override;
    void Initialize() override;
    void PrepareFrame() override;
    void Execute(GDX12CommandList*) override;
    void ClearDenendencies() override;
    void Submit(std::vector<GDX12PublishedReflectionProbe> probes);
    void BindCompute(GDX12CommandList*, UINT globalsCB, UINT metadataSRV, UINT cubesTable);
    void BindGraphics(GDX12CommandList*, UINT globalsCB, UINT metadataSRV, UINT cubesTable);
private:
    struct Data;
    std::unique_ptr<Data> _data;
};
