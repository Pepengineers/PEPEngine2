#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"

// Inputs: depth, world normal/roughness, global prefiltered environment, F0/baked mask.
// Output: RGB probe fallback (-1 if global only), A perceptual roughness for SSSR.
class GDX12ReflectionFallbackPass final : public GDX12RenderPass
{
public:
    GDX12ReflectionFallbackPass();
    ~GDX12ReflectionFallbackPass() override;
    void Initialize() override;
    void Execute(GDX12CommandList*) override;
    void Resize() override;
    void ClearDenendencies() override;
private:
    struct Data;
    std::unique_ptr<Data> _data;
};
