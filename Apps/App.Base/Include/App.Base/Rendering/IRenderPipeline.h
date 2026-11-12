#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include "Engine.RendererDX12/GDX12RenderGeometrySource.h"
#include "Engine.Core/Jobs/JobSystem.h"
#include <utility>

class RenderModule;
class GDX12SwapChain;
class GDX12ReconstructionPass;
struct RenderPipelineContext
{
    RenderModule& Runtime;
    GDX12DeviceResources& Primary;
    RenderPipelineCommonData& Common;
    GDX12SwapChain* BackBuffer;

    JobHandle PrepareScene();
    void Resize();
};

struct RenderPipelineBindings
{
    std::vector<GDX12RenderPass*> PrimaryPasses;
    GDX12ReconstructionPass* Reconstruction = nullptr;
    bool FrameGenerationInputs = false;
    bool UseAsyncCompute = false;
};

struct RenderPipelineGraphInputs
{
    IRenderPassLink* BackBuffer;
};

class IRenderPipeline
{
public:
    virtual ~IRenderPipeline() = default;
    virtual void Initialize(RenderPipelineContext&) = 0;
    virtual bool AllowsFrameGeneration() const { return true; }
    virtual void PrepareMaterialBindings(RenderPipelineContext&) {}
    virtual void ResetWorldHistory() {}
    virtual std::pair<size_t, size_t> GetWorldProbeReadiness() const { return {}; }
#ifdef _DEBUG
    virtual void DrawDebugControls(const RenderModule&) {}
#endif
    // Before frame constants/camera uploads, so a changed render grid applies to the same frame.
    virtual void UpdateFrameSettings(RenderPipelineContext&) {}
    // Called once after the frame-slot wait; true requests recompiling the graph.
    virtual bool PrepareFrame(RenderPipelineContext&, GDX12CommandList*) = 0;
    virtual void BuildGraph(GDX12RenderGraph&, const RenderPipelineGraphInputs&) = 0;
    virtual void Resize(RenderPipelineContext&) = 0;
    virtual const RenderPipelineBindings& GetBindings() const = 0;
};
