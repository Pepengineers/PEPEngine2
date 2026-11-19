#pragma once
#include "../Graph/RenderGraph.h"
#include "Engine.RendererDX12/GDX12DeviceResources.h"

namespace BistroRender {
// The scene owner drains the frame executor before replacing textures.
class CloudPictureTexture {
public:
    CloudPictureTexture(GDX12DeviceResources& primary, UINT width, UINT height);
    ~CloudPictureTexture();
    void Prepare(UINT frameSlot, double totalTime);
    GDX12Texture* Texture(UINT frameSlot) const;
    void Append(Graph&, std::vector<ID3D12Resource*>&);
    bool Record(Id, ID3D12GraphicsCommandList*);
private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};
}
