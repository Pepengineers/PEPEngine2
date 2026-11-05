#include "App.Base/Rendering/CommonRenderPasses.h"

CommonRenderPasses::CommonRenderPasses(GDX12SwapChain* backBuffer, GDX12WBOITTransparencyPass::DepthPartition partition)
    : Transparency(partition), FrameGeneration(backBuffer)
{
    Bindings.PrimaryPasses = { &Environment, &Culling, &Depth, &ParticleSimulation, &SunShadow,
        &LocalShadow, &Reflections, &Opaque, &AO, &Lighting, &ReflectionPreview, &SSR,
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
    for (auto* pass : Bindings.SecondaryPasses) pass->ClearDenendencies();
}

void CommonRenderPasses::Setup(RenderPipelineContext& context)
{
    for (auto* pass : Bindings.PrimaryPasses) pass->Setup(&context.Primary, &context.Secondary, &context.Common);
    for (auto* pass : Bindings.SecondaryPasses) pass->Setup(&context.Secondary, &context.Primary, &context.Common);
}

void CommonRenderPasses::Initialize()
{
    auto passes = Bindings.PrimaryPasses;
    passes.insert(passes.end(), Bindings.SecondaryPasses.begin(), Bindings.SecondaryPasses.end());
    GDX12RenderPass::InitializeAll(std::move(passes));
}

void CommonRenderPasses::Resize(RenderPipelineContext& context)
{
    Reconstruction.QueryRenderTargetResolution();
    for (auto* pass : Bindings.PrimaryPasses) pass->Resize();
    for (auto* pass : Bindings.SecondaryPasses) pass->Resize();
}

void CommonRenderPasses::BindAmbientOcclusion(RenderPipelineCommonData& common, bool early)
{
    common.AOAfterDepthPrepass = early;
    AO.SetInputs({ Depth.GetOutput(2), (early ? Depth : Opaque).GetOutput(3) });
}

void CommonRenderPasses::PrepareFrame(RenderPipelineContext& context, GDX12CommandList* commands)
{
    Bindings.UseAsyncCompute = GetParticleSettings().AsyncCompute;
    if (!context.Primary.ParticleSimulation)
        context.Primary.ParticleSimulation = std::make_unique<GDX12ParticleSimulation>();
    context.Primary.ParticleSimulation->BeginFrame(&context.Primary, context.Common);
    Particles.BeginFrame();
    context.Primary.ParticleSimulation->InitializePools(commands, &context.Primary);
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
    SunShadow.Prepare();
    LocalShadow.Prepare();
    Reflections.Prepare();
    context.Primary.PrepareLocalShadowData();
    context.Primary.UploadLocalShadowData(commands);
}
