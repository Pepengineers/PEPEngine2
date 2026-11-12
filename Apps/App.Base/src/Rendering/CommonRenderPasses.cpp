#include "App.Base/Rendering/CommonRenderPasses.h"
#include "App.Base/Modules/RenderModule.h"
#include "App.Base/Rendering/CrossAdapterAO.h"
#include "App.Base/Rendering/RenderShadowRoutes.h"
#include "App.Base/Rendering/CrossAdapterParticles.h"

CommonRenderPasses::CommonRenderPasses(GDX12SwapChain* backBuffer, GDX12WBOITTransparencyPass::DepthPartition partition)
    : Transparency(partition), FrameGeneration(backBuffer)
{
    Bindings.PrimaryPasses = { &Environment, &Culling, &Depth, &ParticleSimulation, &SunShadow,
        &LocalShadow, &Reflections, &Opaque, &AO, &Lighting, &SSR,
        &Transparency, &Particles, &Composition, &DepthOfField, &MotionBlur,
        &Reconstruction, &Output, &FrameGeneration };
    Bindings.Reconstruction = &Reconstruction;
    Bindings.FrameGenerationInputs = true;
    for (auto* pass : std::array<GDX12RenderPass*, 6>{ &Depth, &Opaque, &AO, &Lighting, &Transparency, &Composition })
        pass->SetFlag(RENDER_PASS_FLAG_USE_DOWNSCALED_RESOLUTION, true);
}

CommonRenderPasses::~CommonRenderPasses()
{
    for (auto* pass : Bindings.PrimaryPasses) pass->ClearDenendencies();
}

void CommonRenderPasses::Setup(RenderPipelineContext& context)
{
    for (auto* pass : Bindings.PrimaryPasses) pass->Setup(&context.Primary, &context.Secondary, &context.Common);
    if (auto* runtime = context.Runtime.GetMGPURuntime())
        if (auto* secondary = context.Runtime.GetMGPUDevices().Secondary)
            _placedParticles = std::make_unique<CrossAdapterParticles>(context.Primary, *secondary, *runtime,
                ParticleSimulation, Particles);
}

void CommonRenderPasses::AddParticleSimulation(GDX12RenderGraph& graph)
{
    if (_placedParticles) _placedParticles->Configure(graph, Bindings, Depth.GetOutput(2));
    else graph.AddPass(ParticleSimulation);
}

void CommonRenderPasses::ResetWorldHistory()
{
    if (_placedParticles) _placedParticles->ResetWorld();
    SunShadow.ResetUpdateSchedule();
    LocalShadow.ResetUpdateSchedule();
    if (_placedShadows) _placedShadows->Invalidate();
    Reflections.ResetHistory();
}

void CommonRenderPasses::Initialize()
{
    GDX12RenderPass::InitializeAll(Bindings.PrimaryPasses);
}

void CommonRenderPasses::Resize(RenderPipelineContext& context)
{
    if (_placedShadows) _placedShadows->Invalidate();
    Reconstruction.QueryRenderTargetResolution();
    for (auto* pass : Bindings.PrimaryPasses) pass->Resize();
}

void CommonRenderPasses::BindAmbientOcclusion(RenderPipelineCommonData& common, bool early)
{
    common.AOAfterDepthPrepass = early;
    AO.SetInputs({ Depth.GetOutput(2), (early ? Depth : Opaque).GetOutput(3) });
}

bool CommonRenderPasses::UpdateAmbientOcclusionRoute(RenderPipelineContext& context)
{
    auto* runtime = context.Runtime.GetMGPURuntime();
    auto* secondary = context.Runtime.GetMGPUDevices().Secondary;
    const bool enabled = IsMultiGPUAmbientOcclusionEnabled() && runtime && secondary;
    if (enabled && !_placedAO)
        _placedAO = std::make_unique<CrossAdapterAO>(AO, context.Primary, *secondary, *runtime);
    const bool changed = _usePlacedAO != enabled;
    _usePlacedAO = enabled;
    return changed;
}

void CommonRenderPasses::AddAmbientOcclusion(GDX12RenderGraph& graph, bool early)
{
    if (!_usePlacedAO)
    {
        graph.AddPass(AO);
        return;
    }
    graph.AddPass(*_placedAO);
    graph.SetLegacySplit(*_placedAO, { Depth.GetOutput(2), (early ? Depth : Opaque).GetOutput(3) }, { AO.GetOutput(0) });
    Bindings.LegacyReplacements.push_back({ _placedAO.get(),
        [this](const auto& boundary, auto prefix) { return _placedAO->Submit(boundary, prefix); },
        [this](auto tail) { _placedAO->RetireFrame(tail); } });
}

GDX12Submission::CompletionPoint CommonRenderPasses::GetLastAmbientOcclusionFinalize() const
{
    return _placedAO ? _placedAO->GetLastFinalize() : GDX12Submission::CompletionPoint{};
}

void CommonRenderPasses::AddShadowRoutes(GDX12RenderGraph& graph, GDX12RenderPass& sunNode, GDX12RenderPass& localNode)
{
    if (!_usePlacedShadows) return;
    _placedShadows->Configure(graph, sunNode, GDX12ShadowAtlas::Sun, Bindings);
    _placedShadows->Configure(graph, localNode, GDX12ShadowAtlas::Local, Bindings);
}

bool CommonRenderPasses::PrepareFrame(RenderPipelineContext& context, GDX12CommandList* commands)
{
    EnsureShadowUpdatePolicies(context.Primary.LocalLights);
    auto& display = *context.Primary.FrameConstants[context.Primary.CurrFrameConstantsIndex];
    display.DisplayShadowPolicy = IsShadowPolicyEnabled();
    if (display.DisplayShadowPolicy)
    {
        display.DisplayShadowCB->CopyData(0, context.Primary.ShadowSettings);
        display.DisplayShadowLights->Resize((std::max)(1u, static_cast<UINT>(context.Primary.LocalLights.size())));
        if (!context.Primary.LocalLights.empty())
            display.DisplayShadowLights->CopyData(0, context.Primary.LocalLights.data(), static_cast<UINT>(context.Primary.LocalLights.size()));
    }
    Bindings.UseAsyncCompute = GetParticleSettings().AsyncCompute || IsAmbientOcclusionAsyncComputeEnabled();
    if (!context.Primary.ParticleSimulation)
        context.Primary.ParticleSimulation = std::make_unique<GDX12ParticleSimulation>();
    if (!_placedParticles || !_placedParticles->PrepareFrame(context.Common))
    {
        context.Primary.ParticleSimulation->BeginFrame(&context.Primary, context.Common);
        context.Primary.ParticleSimulation->InitializePools(commands, &context.Primary);
    }
    Particles.BeginFrame();
    GDX12_SCOPED_CPU_EVENT("Scene preparation");
    auto visibility = context.PrepareScene();
    try
    {
        Lighting.Prepare();
        GDX12_SCOPED_CPU_EVENT("Shadow visibility wait");
        visibility.Complete();
    }
    catch (...)
    {
        try { visibility.Complete(); } catch (...) {}
        throw;
    }
    Depth.Prepare();
    Opaque.Prepare();
    if (_placedShadows) _placedShadows->UpdateQuality();
    SunShadow.Prepare();
    LocalShadow.Prepare();
    Reflections.Prepare();
    context.Primary.PrepareLocalShadowData();
    context.Primary.UploadLocalShadowData(commands);

    auto* runtime = context.Runtime.GetMGPURuntime();
    auto* secondary = context.Runtime.GetMGPUDevices().Secondary;
    const bool enabled = IsMultiGPUShadowsEnabled() && runtime && secondary;
    if (enabled)
    {
        if (!_placedShadows)
            _placedShadows = std::make_unique<RenderShadowRoutes>(context.Primary, *secondary, *runtime, SunShadow, LocalShadow);
        _placedShadows->PrepareFrame(context.Runtime);
    }
    else if (_usePlacedShadows)
        _placedShadows->Invalidate();
    return std::exchange(_usePlacedShadows, enabled) != enabled;
}
