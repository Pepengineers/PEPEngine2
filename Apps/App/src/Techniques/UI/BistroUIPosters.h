#pragma once

#include "Graph/RenderGraphExecutor.h"
#include <cstddef>
#include <memory>

class GDX12DeviceResources;
class RenderModule;
class SceneManagerModule;
class GDX12Texture;

namespace BistroRender
{
struct NativeWork;
struct CanvasRead
{
    Access Texture;
    UINT MaterialIndex, TextureIndex;
};
}
class BistroUIPosters final
{
public:
    static constexpr std::size_t Count = 3;

    BistroUIPosters(RenderModule& render, const std::shared_ptr<SceneManagerModule>& scenes, bool cloudCanvas = false);
    ~BistroUIPosters();
    BistroUIPosters(const BistroUIPosters&) = delete;
    BistroUIPosters& operator=(const BistroUIPosters&) = delete;

    void PrepareFrame(GDX12DeviceResources&, BistroRender::Executor&, uint64_t frame, double animationSeconds, GDX12Texture* image);
    std::vector<BistroRender::CanvasRead> Append(BistroRender::Graph&, std::vector<ID3D12Resource*>&, BistroRender::NativeWork&);
    void AddExchanges(const BistroRender::Schedule&, std::vector<BistroRender::PacketWait>&,
                      std::vector<BistroRender::Executor::PacketExchange>&);
    void Submitted(uint64_t frame, const std::vector<BistroRender::CompletionToken>&);

private:
    struct Impl;
    std::shared_ptr<Impl> _impl;
};
