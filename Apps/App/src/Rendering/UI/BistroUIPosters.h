#pragma once

#include "Engine.RendererDX12/GDX12RenderGraph.h"
#include <cstddef>
#include <memory>

class GDX12DeviceResources;
class RenderModule;
class SceneManagerModule;

class BistroUIPosters final
{
public:
    static constexpr std::size_t Count = 2;

    BistroUIPosters(RenderModule& render, const std::shared_ptr<SceneManagerModule>& scenes);
    ~BistroUIPosters();
    BistroUIPosters(const BistroUIPosters&) = delete;
    BistroUIPosters& operator=(const BistroUIPosters&) = delete;

    // Call after waiting the primary frame slot and before material uploads.
    // True requests recompiling the graph.
    bool PrepareFrame(GDX12DeviceResources& primary, UINT frameSlot, double animationSeconds);

    GDX12RenderPass& RasterPass(std::size_t index) const;
    IRenderPassLink& Output(std::size_t index) const;
    void AddPasses(GDX12RenderGraph& graph) const;
    void DeclareMaterialReads(GDX12RenderGraphBuilder& builder) const;
    bool UsesAsyncCompute() const;

private:
    struct Impl;
    std::shared_ptr<Impl> _impl;
};
