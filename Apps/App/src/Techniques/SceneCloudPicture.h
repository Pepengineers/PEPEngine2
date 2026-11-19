#pragma once
#include "CloudPictureTexture.h"
#include "App.Base/Rendering/IRenderPipeline.h"
#include <functional>

namespace BistroRender {
class SceneCloudPicture {
public:
    explicit SceneCloudPicture(RenderPipelineContext&);
    ~SceneCloudPicture();
    void UpdateSettings(const std::function<void()>& drain);
    void Prepare(UINT slot, double time);
    GDX12Texture* Image() const;
    void Append(Graph&, std::vector<ID3D12Resource*>&);
    bool Record(Id, ID3D12GraphicsCommandList*);
private:
    struct Impl;
    std::shared_ptr<Impl> _impl;
};
}
