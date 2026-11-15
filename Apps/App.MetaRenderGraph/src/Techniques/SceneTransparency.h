#pragma once
#include "NativeWork.h"
#include "SceneGeometry.h"
#include "SceneEnvironment.h"
#include "SceneParticles.h"
#include "SceneWater.h"
#include "Engine.RendererDX12/RenderPasses/GDX12WBOITCompositionPass.h"

namespace Meta
{
class SceneTransparency
{
public:
    SceneTransparency(GDX12DeviceResources&, RenderPipelineCommonData&);
    ~SceneTransparency();
    void PrepareWater(Graph&, std::vector<ID3D12Resource*>&, SceneGeometry&, UINT slot);
    void SetWaterRetirement(std::function<void()> retire) { _water.SetRetirement(std::move(retire)); }
    GDX12Texture* Append(Graph&, std::vector<ID3D12Resource*>&, SceneGeometry&, UINT slot,
        GDX12Texture* hdr, GDX12Texture* sun, GDX12Texture* local, SceneEnvironment&);
    bool Record(Id id, ID3D12GraphicsCommandList* cmd) const { return _work.Record(id, cmd) || _water.Record(id, cmd) || _particles.Record(id, cmd); }
    GDX12Texture* ParticleMotion() const { return _particles.MotionOutput(); }
    GDX12Texture* Depth() const { return _water.Depth(); }
    GDX12Texture* Motion() const { return _water.Motion(); }
private:
    struct Phase
    {
        GDX12WBOITTransparencyPass Draw;
        GDX12WBOITCompositionPass Compose;
        bool Initialized = false;
        explicit Phase(GDX12GeometryDepthPartition partition = GDX12GeometryDepthPartition::All) : Draw(partition) {}
    };
    struct Frame { Phase All, Far{GDX12GeometryDepthPartition::Far}, Near{GDX12GeometryDepthPartition::Near}; };
    GDX12Texture* AppendPhase(Graph&, std::vector<ID3D12Resource*>&, SceneGeometry&, UINT slot, Phase&,
        GDX12Texture* hdr, GDX12Texture* sun, GDX12Texture* local, SceneEnvironment&, bool particles,
        Id predecessor = Invalid);
    GDX12DeviceResources& _resources;
    RenderPipelineCommonData& _common;
    std::vector<Frame> _frames;
    NativeWork _work;
    SceneParticles _particles;
    SceneWater _water;
};
}
