#pragma once
#include "App.Base/Rendering/IRenderPipeline.h"
#include "Engine.RendererDX12/RenderPasses/GDX12GPUCullingPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12CascadedShadowPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12EnvironmentIBLPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12ReflectionProbePass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12OpaquePass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12LocalShadowPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12DeferredLightingPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12AmbientOcclusionPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12SSSRPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12WBOITTransparencyPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12WBOITCompositionPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12ParticlesPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12OutputToScreenPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12ReconstructionPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12FrameGenerationPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12DepthOfFieldPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12MotionBlurPass.h"

class CrossAdapterAO;
class RenderShadowRoutes;
class CrossAdapterParticles;
class CrossAdapterReflections;

// Owns reusable techniques and their lifecycle; each pipeline chooses links and graph order.
struct CommonRenderPasses
{
    explicit CommonRenderPasses(GDX12SwapChain*,
        GDX12WBOITTransparencyPass::DepthPartition = GDX12WBOITTransparencyPass::DepthPartition::All);
    ~CommonRenderPasses();
    void Setup(RenderPipelineContext&);
    void Initialize();
    void Resize(RenderPipelineContext&);
    bool PrepareFrame(RenderPipelineContext&, GDX12CommandList*);
    RenderShadowRoutes* GetShadowRoutes() const { return _usePlacedShadows ? _placedShadows.get() : nullptr; }
    void AddShadowRoutes(GDX12RenderGraph&, GDX12RenderPass& sunNode, GDX12RenderPass& localNode);
    void AddParticleSimulation(GDX12RenderGraph&);
    void ResetWorldHistory();
    CrossAdapterParticles* GetParticleRoute() const { return _placedParticles.get(); }
    CrossAdapterReflections* GetReflectionRoute() const { return _placedReflections.get(); }
    void AddReflectionRoute(GDX12RenderGraph&, GDX12RenderPass& node, std::vector<IRenderPassLink*> materialInputs = {});
    void BindAmbientOcclusion(RenderPipelineCommonData&, bool early);
    bool UpdateAmbientOcclusionRoute(RenderPipelineContext&);
    void AddAmbientOcclusion(GDX12RenderGraph&, bool early);
    GDX12Submission::CompletionPoint GetLastAmbientOcclusionFinalize() const;

    GDX12EnvironmentIBLPass Environment;
    GDX12GPUCullingPass Culling;
    GDX12OpaquePass Depth{ GDX12OpaquePass::Stage::DepthPrepass };
    GDX12ParticleSimulationPass ParticleSimulation;
    GDX12CascadedShadowPass SunShadow;
    GDX12LocalShadowPass LocalShadow;
    GDX12ReflectionProbePass Reflections;
    GDX12OpaquePass Opaque{ GDX12OpaquePass::Stage::GBuffer };
    GDX12AmbientOcclusionPass AO;
    GDX12DeferredLightingPass Lighting;
    GDX12SSSRPass SSR;
    GDX12WBOITTransparencyPass Transparency;
    GDX12ParticlesPass Particles;
    GDX12WBOITCompositionPass Composition;
    GDX12DepthOfFieldPass DepthOfField;
    GDX12MotionBlurPass MotionBlur;
    GDX12ReconstructionPass Reconstruction;
    GDX12OutputToScreenPass Output;
    GDX12FrameGenerationPass FrameGeneration;
    RenderPipelineBindings Bindings;

private:
    friend class BistroRenderGraphTests;
    std::unique_ptr<CrossAdapterAO> _placedAO;
    bool _usePlacedAO = false;
    std::unique_ptr<RenderShadowRoutes> _placedShadows;
    bool _usePlacedShadows = false;
    std::unique_ptr<CrossAdapterParticles> _placedParticles;
    std::unique_ptr<CrossAdapterReflections> _placedReflections;
};
