#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include "Engine.RendererDX12/GDX12RenderGeometrySource.h"
#include "Engine.Core/Jobs/JobSystem.h"
#include <functional>

class RenderModule;
class GDX12SwapChain;
class GDX12ReconstructionPass;
namespace Engine::UI
{
    enum class CanvasId : std::uint64_t;
    class CrossAdapterUI;
}

struct RenderPipelineContext
{
    RenderModule& Runtime;
    GDX12DeviceResources& Primary;
    GDX12DeviceResources& Secondary;
    RenderPipelineCommonData& Common;
    GDX12SwapChain* BackBuffer;

    JobHandle PrepareScene();
    void Resize();
};

struct LegacyReplacementRoute
{
    GDX12RenderPass* Pass;
    GDX12RenderGraph::SubmitLegacyReplacement Submit;
    std::function<void(GDX12Submission::CompletionPoint)> Retire;
    std::function<void(GDX12CommandList&)> RecordTail;
};

struct RenderPipelineBindings
{
    std::vector<GDX12RenderPass*> PrimaryPasses;
    GDX12ReconstructionPass* Reconstruction = nullptr;
    bool FrameGenerationInputs = false;
    bool UseAsyncCompute = false;
    std::vector<LegacyReplacementRoute> LegacyReplacements;
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
    virtual std::vector<Engine::UI::CanvasId> GetWorldCanvasIds() const { return {}; }
    virtual void PrepareMaterialBindings(RenderPipelineContext&, Engine::UI::CrossAdapterUI*) {}
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
