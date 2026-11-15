#pragma once
#include "NativeWork.h"
#include "SceneGeometry.h"
#include "SceneEnvironment.h"
#include "SceneParticles.h"
#include "Engine.RendererDX12/RenderPasses/GDX12WBOITCompositionPass.h"

namespace Meta
{
class SceneTransparency
{
public:
    SceneTransparency(GDX12DeviceResources&, RenderPipelineCommonData&);
    ~SceneTransparency();
    GDX12Texture* Append(Graph&, std::vector<ID3D12Resource*>&, SceneGeometry&, UINT slot,
        GDX12Texture* hdr, GDX12Texture* sun, GDX12Texture* local, SceneEnvironment&);
    bool Record(Id id, ID3D12GraphicsCommandList* cmd) const { return _work.Record(id, cmd) || _particles.Record(id, cmd); }
private:
    struct Frame
    {
        GDX12WBOITTransparencyPass Draw;
        GDX12WBOITCompositionPass Compose;
        bool Initialized = false;
    };
    GDX12DeviceResources& _resources;
    RenderPipelineCommonData& _common;
    std::vector<Frame> _frames;
    NativeWork _work;
    SceneParticles _particles;
};
}
