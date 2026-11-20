#pragma once
#include "CloudPictureTexture.h"
#include "App.Base/Rendering/IRenderPipeline.h"
#include "../Graph/RenderGraphExecutor.h"

namespace BistroRender {
class SceneCloudPicture {
public:
    explicit SceneCloudPicture(RenderPipelineContext&);
    ~SceneCloudPicture();
    void UpdateSettings(std::unique_ptr<Executor>& executor);
    void Prepare(UINT slot, double time);
    GDX12Texture* Image() const;
    bool IsRemote() const;
    void Append(Graph&, std::vector<ID3D12Resource*>&);
    bool Record(Id, ID3D12GraphicsCommandList*);
    std::vector<PacketWait> Waits(const Schedule&);
    void Submitted(const std::vector<CompletionToken>&, bool frameSubmitted);
private:
    struct Impl;
    std::shared_ptr<Impl> _impl;
};
}
