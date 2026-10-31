#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12FidelityFXPasses.h"

// Inputs: lit HDR, depth, normal/roughness, F0/baked, AO, prefiltered cube, BRDF LUT, SSR radiance.
class GDX12SpecularResolvePass final : public GDX12RenderPass
{
public:
    explicit GDX12SpecularResolvePass(GDX12SssrPass* reflections);
    ~GDX12SpecularResolvePass() override;
    void Initialize() override;
    void Execute(GDX12CommandList* commands) override;
    void Resize() override;
    void ClearDenendencies() override;
private:
    struct Data;
    std::unique_ptr<Data> _data;
    GDX12SssrPass* _reflections;
};
