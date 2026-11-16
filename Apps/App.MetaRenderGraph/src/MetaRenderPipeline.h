#pragma once
#include "App.Base/Rendering/IRenderPipeline.h"
#include "Techniques/SceneGeometry.h"
#include "Techniques/SceneEnvironment.h"
#include "Techniques/SceneLighting.h"
#include "Techniques/SceneWaterParticles.h"
#include "Techniques/ScenePostUI.h"
#include "Techniques/SceneCloudPicture.h"
#include "Graph/CrossAdapterTransfer.h"
#include "Techniques/WaterTransfer.h"
#include "Techniques/ParticleTransfer.h"
#include "Engine.RendererDX12/RenderPasses/GDX12GPUCullingPass.h"
#include "Common/GameTimer.h"
class MetaRenderPipeline final : public IRenderPipeline
{
  public:
    explicit MetaRenderPipeline(bool verify = false, bool opaqueOnly = false)
        : _verify(verify), _diagnosticReadbacks(verify), _opaqueOnly(opaqueOnly)
    {
    }
    void SetVerification(bool verify)
    {
        if (verify && (_waterTransfer || _particleTransfer)) throw std::invalid_argument("Meta.Water cannot run single-GPU verification mode");
        if (_verify != verify && _lighting)
        {
            _lighting->Reset();
            if (_waterParticles) _waterParticles->Reset();
            if (_post) _post->Reset();
        }
        _verify = verify;
        _diagnosticReadbacks |= verify;
    }
    void ValidateQueued();
    bool OwnsScreenUI() const override { return true; }
    bool AdvancesParticlesWhileWorldPaused() const override { return false; }
    bool AdvancesSkeletalMeshesWhileWorldPaused() const override { return false; }
    void PrepareMaterialBindings(RenderPipelineContext& c) override
    {
        _executor->Begin(c.Primary.CurrFrameConstantsIndex);
        _animationTime += (std::max)(0.0, double(c.Common.GameTimer->DeltaTime()));
        if (_cloudPicture) {
            if (_cloudPicture->UpdateSettings([this] { _executor->Drain(); _executor.reset(); })) {
                _executor = std::make_unique<Meta::Executor>(_cloudPicture->Submissions(), 0,
                    UINT(c.Primary.FrameConstants.size()), 1024);
                _executor->Begin(c.Primary.CurrFrameConstantsIndex);
            }
            _cloudPicture->Prepare(c.Primary.CurrFrameConstantsIndex, _animationTime);
        }
        _post->PrepareMaterials(_animationTime, _cloudPicture ? _cloudPicture->Image() : nullptr);
    }
    void DrainExecution() override {
        if (_executor) _executor->Drain();
        if (_cloudPicture) _cloudPicture->Drain();
        if (_crossAdapter) _crossAdapter->Stop();
        if (_waterTransfer) _waterTransfer->Stop();
        if (_particleTransfer) _particleTransfer->Stop();
        if (_post) _post->ReleasePresentation();
    }
    void UpdateFrameSettings(RenderPipelineContext& c) override
    {
        _post->UpdateSettings();
        if (!(GetReconstructionSettings() == _post->Reconstruction().GetStatus().Requested) || _post->NeedsResize())
            DrainExecution();
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
        return !_opaqueOnly && !_verify && _post && _post->AllowsFrameGeneration();
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
    std::unique_ptr<Meta::SceneCloudPicture> _cloudPicture;
    std::unique_ptr<Meta::CrossAdapterTransfer> _crossAdapter;
    std::shared_ptr<Meta::SimulationDomain> _simulationDomain;
    std::unique_ptr<Meta::WaterTransfer> _waterTransfer;
    std::unique_ptr<Meta::ParticleTransfer> _particleTransfer;
    std::unique_ptr<Meta::Executor> _executor;
    Meta::Schedule _schedule;
    uint32_t _frame = 0;
    double _animationTime = 0;
    bool _verify = false;
    bool _opaqueOnly = false;
    // The diagnostic host also reads queued frames after paired comparison ends.
    bool _diagnosticReadbacks = false;
    bool _cloudsEnabled = false;
    bool _environmentTestsRun = false;
};
