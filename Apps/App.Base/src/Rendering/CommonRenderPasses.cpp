#include "App.Base/Rendering/CommonRenderPasses.h"
#include "App.Base/Modules/RenderModule.h"

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
    for (auto* pass : Bindings.PrimaryPasses) pass->Setup(&context.Primary, &context.Common);
}

void CommonRenderPasses::AddParticleSimulation(GDX12RenderGraph& graph)
{
    graph.AddPass(ParticleSimulation);
}

void CommonRenderPasses::ResetWorldHistory()
{
    Reflections.ResetHistory();
}

void CommonRenderPasses::Initialize()
{
    GDX12RenderPass::InitializeAll(Bindings.PrimaryPasses);
}

void CommonRenderPasses::Resize(RenderPipelineContext& context)
{
    Reconstruction.QueryRenderTargetResolution();
    for (auto* pass : Bindings.PrimaryPasses) pass->Resize();
}

void CommonRenderPasses::BindAmbientOcclusion(RenderPipelineCommonData& common, bool early)
{
    common.AOAfterDepthPrepass = early;
    AO.SetInputs({ Depth.GetOutput(2), (early ? Depth : Opaque).GetOutput(3) });
}

void CommonRenderPasses::AddAmbientOcclusion(GDX12RenderGraph& graph)
{
    graph.AddPass(AO);
}

void CommonRenderPasses::PrepareFrame(RenderPipelineContext& context, GDX12CommandList* commands)
{
    Bindings.UseAsyncCompute = GetParticleSettings().AsyncCompute || IsAmbientOcclusionAsyncComputeEnabled();
    if (!context.Primary.ParticleSimulation)
        context.Primary.ParticleSimulation = std::make_unique<GDX12ParticleSimulation>();
    context.Primary.ParticleSimulation->BeginFrame(&context.Primary, context.Common);
    context.Primary.ParticleSimulation->InitializePools(commands, &context.Primary);
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
    SunShadow.Prepare();
    LocalShadow.Prepare();
    Reflections.Prepare();
    context.Primary.PrepareLocalShadowData();
    context.Primary.UploadLocalShadowData(commands);

}
