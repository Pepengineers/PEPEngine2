#pragma once
#include "../Graph/RenderGraphExecutor.h"
#include "Engine.RendererDX12/GDX12DeviceResources.h"

namespace BistroRender {
// One immutable grid/adapter generation; the scene owner drains before replacement.
class CloudPictureTexture {
public:
    CloudPictureTexture(GDX12DeviceResources& primary, UINT width, UINT height, int adapter);
    ~CloudPictureTexture();
    const std::shared_ptr<QueueSubmission>& Submissions() const;
    void Prepare(UINT frameSlot, double totalTime);
    GDX12Texture* Texture(UINT frameSlot) const;
    void Append(Graph&, std::vector<ID3D12Resource*>&);
    bool Record(Id, ID3D12GraphicsCommandList*);
    std::vector<PacketWait> Waits(const Schedule&) const;
    void RetireFrame(const Schedule&, const std::vector<CompletionToken>&, bool success);
    void Drain();
private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};
}
