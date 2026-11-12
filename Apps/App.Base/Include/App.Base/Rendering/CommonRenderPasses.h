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


// Owns reusable techniques and their lifecycle; each pipeline chooses links and graph order.
struct CommonRenderPasses
{
    explicit CommonRenderPasses(GDX12SwapChain*,
        GDX12WBOITTransparencyPass::DepthPartition = GDX12WBOITTransparencyPass::DepthPartition::All);
    ~CommonRenderPasses();
    void Setup(RenderPipelineContext&);
    void Initialize();
    void Resize(RenderPipelineContext&);
    void PrepareFrame(RenderPipelineContext&, GDX12CommandList*);
    void AddParticleSimulation(GDX12RenderGraph&);
    void ResetWorldHistory();
    void BindAmbientOcclusion(RenderPipelineCommonData&, bool early);
    void AddAmbientOcclusion(GDX12RenderGraph&);

    GDX12EnvironmentIBLPass Environment;
    GDX12GPUCullingPass Culling;
    GDX12OpaquePass Depth{ GDX12OpaquePass::Stage::DepthPrepass };
    GDX12ParticleSimulationPass ParticleSimulation;
    GDX12ParticleOffloadPass ParticleOffload;
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

};
