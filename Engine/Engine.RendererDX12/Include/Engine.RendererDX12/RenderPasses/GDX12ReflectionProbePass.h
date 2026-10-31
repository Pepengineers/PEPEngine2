#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include "Engine.RendererDX12/GDX12ReflectionProbes.h"

// Inputs: global irradiance, prefiltered environment, BRDF LUT, sky radiance, local shadow array.
class GDX12ReflectionProbePass final : public GDX12RenderPass
{
public:
    GDX12ReflectionProbePass();
    ~GDX12ReflectionProbePass() override;
    void Initialize() override;
    void PrepareFrame() override;
    void Execute(GDX12CommandList*) override;
    void ClearDenendencies() override;
    void Submit(uint64_t world, std::vector<GDX12ReflectionProbeDesc> probes,
        uint64_t staticRevision, uint64_t shadingRevision);
    // Metadata and texture describe the same completed generation; the list is refreshed each frame.
    const std::vector<GDX12PublishedReflectionProbe>& GetPublished() const;
    UINT GetLastFrameStaticFaces() const;
private:
    struct Data;
    std::unique_ptr<Data> _data;
};
