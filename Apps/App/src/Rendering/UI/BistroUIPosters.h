#pragma once

#include "Engine.RendererDX12/GDX12RenderGraph.h"
#include <cstddef>
#include <memory>
#include <optional>

class GDX12DeviceResources;
class RenderModule;
class SceneManagerModule;
namespace Engine::UI { class CrossAdapterUI; }

class BistroUIPosters final
{
public:
    static constexpr std::size_t Count = 2;

    BistroUIPosters(RenderModule& render, const std::shared_ptr<SceneManagerModule>& scenes);
    ~BistroUIPosters();
    BistroUIPosters(const BistroUIPosters&) = delete;
    BistroUIPosters& operator=(const BistroUIPosters&) = delete;

    // Call after waiting the primary frame slot, after controller BeginFrame, and
    // before material uploads. True requests recompiling the legacy graph.
    bool PrepareFrame(GDX12DeviceResources& primary, UINT frameSlot, double animationSeconds,
        Engine::UI::CrossAdapterUI* controller);

    GDX12RenderPass& RasterPass(std::size_t index) const;
    IRenderPassLink& Output(std::size_t index) const;
    void AddPasses(GDX12RenderGraph& graph) const;
    void DeclareMaterialReads(GDX12RenderGraphBuilder& builder) const;
    bool UsesAsyncCompute() const;
    bool HasRoutes() const;
    bool HandlesReplacement(const GDX12RenderGraph::LegacyBoundary& boundary) const;
    std::optional<GDX12Submission::CompletionPoint> SubmitReplacement(
        const GDX12RenderGraph::LegacyBoundary& boundary, GDX12Submission::CompletionPoint prefix);

private:
    struct Impl;
    std::shared_ptr<Impl> _impl;
};
