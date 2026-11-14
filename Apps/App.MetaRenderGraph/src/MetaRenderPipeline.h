#pragma once
#include "App.Base/Rendering/IRenderPipeline.h"
#include "Techniques/SceneGeometry.h"
#include "Techniques/SceneEnvironment.h"
#include "Techniques/SceneLighting.h"
#include "Techniques/SceneWaterParticles.h"
#include "Techniques/ScenePostUI.h"
#include "Graph/CrossAdapterTransfer.h"
#include "Engine.RendererDX12/RenderPasses/GDX12GPUCullingPass.h"
class MetaRenderPipeline final : public IRenderPipeline
{
  public:
    explicit MetaRenderPipeline(bool verify = false) : _verify(verify)
    {
    }
    void SetVerification(bool verify)
    {
        if (_verify != verify && _lighting)
        {
            _lighting->Reset();
            if (_waterParticles) _waterParticles->Reset();
            if (_post) _post->Reset();
        }
        _verify = verify;
    }
    void ValidateQueued();
    bool OwnsScreenUI() const override { return true; }
    void PrepareMaterialBindings(RenderPipelineContext& c) override
    {
        _executor->Begin(c.Primary.CurrFrameConstantsIndex);
        _post->PrepareMaterials();
    }
    void DrainExecution() override {
        if (_executor) _executor->Drain();
        if (_crossAdapter) _crossAdapter->Stop();
    }
    void UpdateFrameSettings(RenderPipelineContext& c) override
    {
        _post->UpdateSettings();
        if (_post->Reconstruction().ApplySettings()) c.Resize();
    }
    void Initialize(RenderPipelineContext &) override;
    void ResetWorldHistory() override
    {
        if (_waterParticles) _waterParticles->Reset();
        if (_post) _post->Reset();
        if (_environment)
            _environment->ResetClouds();
        if (_lighting)
            _lighting->Reset();
    }
    std::pair<size_t, size_t> GetWorldProbeReadiness() const override
    {
        return _lighting ? _lighting->Readiness() : std::pair<size_t, size_t>{};
    }
    bool AllowsFrameGeneration() const override
    {
        return !_verify && _post && _post->AllowsFrameGeneration();
    }
    bool PrepareFrame(RenderPipelineContext &, GDX12CommandList *) override;
    void BuildGraph(GDX12RenderGraph &, const RenderPipelineGraphInputs &) override;
    void Resize(RenderPipelineContext &) override { _executor->Drain(); _post->Resize(); }
    const RenderPipelineBindings &GetBindings() const override
    {
        return _bindings;
    }
    bool OwnsExecution() const override
    {
        return true;
    }
    bool OwnsScenePreparation() const override
    {
        return true;
    }
    void RebuildExecution(RenderPipelineContext &) override;
    uint64_t ExecuteFrame(RenderPipelineContext &, GDX12CommandList *,
                          const std::function<void(GDX12CommandList *)> &) override;
    void WriteExecution(std::ostream &os) const override
    {
        _schedule.Write(os);
    }

  private:
    RenderPipelineBindings _bindings;
    std::unique_ptr<Meta::SceneGeometry> _geometry;
    std::unique_ptr<Meta::SceneEnvironment> _environment;
    std::unique_ptr<Meta::SceneLighting> _lighting;
    std::unique_ptr<Meta::SceneWaterParticles> _waterParticles;
    GDX12GPUCullingPass _sceneFlags;
    std::unique_ptr<Meta::ScenePostUI> _post;
    std::unique_ptr<Meta::CrossAdapterTransfer> _crossAdapter;
    std::unique_ptr<Meta::Executor> _executor;
    Meta::Schedule _schedule;
    uint32_t _frame = 0;
    bool _verify = false;
    bool _cloudsEnabled = false;
    bool _environmentTestsRun = false;
};
