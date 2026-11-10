#pragma once
#include "Engine.RendererDX12/GDX12RenderGeometrySource.h"
#include "Rendering/Grass/GDX12GrassRenderer.h"
#include "Components/BistroGrassComponents.h"
#include "App.Base/Modules/SceneManagerModule.h"
#include <map>

class RenderModule;

class BistroGrassGeometry final : public IRenderGeometrySource
{
public:
    BistroGrassGeometry(std::shared_ptr<SceneManagerModule> scenes, RenderModule& renderer);
    ~BistroGrassGeometry() override;
    void CollectScene(float deltaSeconds);
    GDX12GrassFieldInputQueue::Tick CaptureFieldInputs(uint64_t sequence, uint64_t resetRevision);
    void PrepareFrame(GDX12DeviceResources&, const RenderPipelineCommonData&,
        const GDX12GrassRenderer::DisplayedFields* fields = nullptr);
    std::shared_ptr<const GDX12GrassRenderer::LocalCheckpoint> PrepareLocalCheckpoint()
    { return _gpu ? _gpu->PrepareLocalCheckpoint() : nullptr; }
    void ResumeLocalSolver(std::shared_ptr<GDX12GrassFieldProducer> solver, uint64_t resetRevision,
        GDX12GrassFieldInputQueue pending)
    { _gpu->ResumeLocalSolver(std::move(solver), resetRevision, std::move(pending)); }
    void PrepareGPU(GDX12CommandList* commands);
    void PrepareView(const GDX12GeometryContext&, UINT patch);
    bool Expand(GDX12CommandList*, const GDX12GeometryContext&, UINT patch);
    void GatherBatches(const GDX12GeometryContext&, std::vector<GDX12RenderBatch>&) const override;
    std::vector<GDX12RenderGraph::BufferAccess> GetGeometryBufferAccesses(GDX12GeometryStage) const override;
    std::vector<GDX12RenderGraph::BufferAccess> PreparationAccesses() const;
    std::vector<GDX12RenderGraph::BufferAccess> ExpansionAccesses(const GDX12GeometryContext&, UINT patch) const;
    GDX12ParticleInteractionField InteractionField() const { return _gpu ? _gpu->InteractionField() : GDX12ParticleInteractionField{}; }
    UINT PatchCount() const { return _gpu ? _gpu->PatchCount() : 0; }
    uint64_t GeometryAllocationBytes() const { return _gpu ? _gpu->GeometryAllocationBytes() : 0; }

private:
    void Subscribe(World& world);
    void Unsubscribe(World& world);
    struct Subscriptions { ListenerHandle GrassCreated, GrassUpdated, WindCreated, WindUpdated; };
    std::shared_ptr<SceneManagerModule> _scenes;
    RenderModule& _renderer;
    ListenerHandle _created = 0, _destroyed = 0;
    std::map<World*, Subscriptions> _subscriptions;
    uint64_t _owner = 0;
    std::optional<uint64_t> _capturedFieldSignature;
    std::vector<GDX12GrassInstance> _grass;
    std::vector<GDX12WindInstance> _wind;
    GDX12GrassWindControls _controls = {};
    std::vector<BistroInteractionSource> _sources;
    float _deltaSeconds = 0;
    std::unique_ptr<GDX12GrassRenderer> _gpu;
};
