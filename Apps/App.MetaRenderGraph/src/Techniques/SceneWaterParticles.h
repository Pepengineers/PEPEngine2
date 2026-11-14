#pragma once
#include "SceneLighting.h"
#include "Rendering/Water/BistroWaterRendering.h"
#include "Engine.RendererDX12/RenderPasses/GDX12ParticlesPass.h"
namespace Meta
{
// All preparation and recording is owner-only in U6. Kernels and shaders are
// the production implementations; execution/submission belongs to Executor.
class SceneWaterParticles
{
public:
    explicit SceneWaterParticles(RenderPipelineContext&);
    ~SceneWaterParticles();
    void Prepare(float delta);
    GDX12Texture* Append(Graph&, std::vector<ID3D12Resource*>&, SceneGeometry&, SceneEnvironment&,
        GDX12Texture* reflectedHDR, GDX12Texture* probes, bool serial);
    bool Record(Id id, ID3D12GraphicsCommandList* commands) const { return _work.Record(id, commands); }
    void Reset();
    void Resize(); // caller drained Executor
    GDX12Texture* Depth() const { return _depth; }
    GDX12Texture* Motion() const { return _motion; }
    GDX12Texture* ParticleMotion() const { return _particleMotion; }
    struct InputGroup { std::vector<uint64_t> Emitters; bool DepthCollision = false; };
    const std::array<InputGroup, 2>& InputGroups() const { return _groups; }
private:
    struct Path;
    GDX12DeviceResources& _resources;
    RenderPipelineCommonData& _common;
    std::shared_ptr<SceneManagerModule> _scenes;
    std::unique_ptr<BistroWaterRendering> _collector;
    std::array<std::unique_ptr<Path>, 2> _paths;
    std::vector<GDX12FluidInstance> _instances;
    std::array<InputGroup, 2> _groups;
    NativeWork _work;
    GDX12Texture *_depth = nullptr, *_motion = nullptr, *_particleMotion = nullptr;
    bool _reset = false;
};
}
