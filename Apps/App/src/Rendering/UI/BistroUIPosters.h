#pragma once

#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include <cstddef>
#include <memory>

class GDX12DeviceResources;
class RenderModule;
class SceneManagerModule;

class BistroUIPosters final
{
public:
    static constexpr std::size_t Count = 3;

    BistroUIPosters(RenderModule& render, const std::shared_ptr<SceneManagerModule>& scenes, bool cloudCanvas = false);
    ~BistroUIPosters();
    BistroUIPosters(const BistroUIPosters&) = delete;
    BistroUIPosters& operator=(const BistroUIPosters&) = delete;

    // Call after waiting the primary frame slot and before material uploads.
    // True requests recompiling the graph.
    bool PrepareFrame(GDX12DeviceResources& primary, UINT frameSlot, double animationSeconds);
    void SetImageInput(GDX12Texture* image);

    GDX12RenderPass& RasterPass(std::size_t index) const;
    IRenderPassLink& Output(std::size_t index) const;
    std::vector<GDX12Texture*> NativeTargets(std::size_t index) const;

private:
    struct Impl;
    std::shared_ptr<Impl> _impl;
};
