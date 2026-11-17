#pragma once
#include "CloudPictureTexture.h"
#include "App.Base/Rendering/IRenderPipeline.h"
#include <functional>

namespace Meta {
class SceneCloudPicture {
public:
    explicit SceneCloudPicture(RenderPipelineContext&);
    ~SceneCloudPicture();
    bool UpdateSettings(const std::function<void()>& drain);
    void Prepare(UINT slot, double time);
    GDX12Texture* Image() const;
    const std::shared_ptr<QueueSubmission>& Submissions() const;
    void Append(Graph&, std::vector<ID3D12Resource*>&);
    bool Record(Id, ID3D12GraphicsCommandList*);
    std::vector<PacketWait> Waits(const Schedule&) const;
    void RetireFrame(const Schedule&, const std::vector<CompletionToken>&, bool);
    void Drain();
private:
    struct Impl;
    std::shared_ptr<Impl> _impl;
};
}
