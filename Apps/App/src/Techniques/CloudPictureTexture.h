#pragma once
#include "../Graph/QueueSubmission.h"
#include "Engine.RendererDX12/GDX12DeviceResources.h"

namespace BistroRender {
enum class CloudPictureMode { Local = -1, PreviousFrame = 0, CurrentFrame = 1, Independent = 2, Count };
// The scene owner drains the frame executor before replacing textures.
class CloudPictureTexture {
public:
    CloudPictureTexture(GDX12DeviceResources& primary, UINT width, UINT height,
                        std::shared_ptr<QueueSubmission>, CloudPictureMode mode);
    ~CloudPictureTexture();
    void Prepare(UINT frameSlot, double totalTime);
    GDX12Texture* Texture(UINT frameSlot) const;
    void Append(Graph&, std::vector<ID3D12Resource*>&);
    bool Record(Id, ID3D12GraphicsCommandList*);
    std::vector<PacketWait> Waits(const Schedule&);
    void Submitted(const std::vector<CompletionToken>&);
    void Presented();
    bool IsRemote() const;
    void Stop();
private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};
}
