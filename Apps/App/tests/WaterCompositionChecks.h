#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12WBOITTransparencyPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12WBOITCompositionPass.h"

// Both routes draw the same frozen simulation pool; only their composition differs.
template<class MakeColor, class Execute>
static void CheckWaterComposition(GDX12Device& device, GDX12DeviceResources& resources,
    RenderPipelineCommonData& common, GDX12Texture& depth, GDX12Texture& motion,
    GDX12EnvironmentIBLPass& environment, MakeColor&& makeColor, Execute&& execute,
    Vector3 eye, Vector3 forward)
{
    const auto oldSkySource = resources.Sky.Source;
    auto background = makeColor(DXGI_FORMAT_R16G16B16A16_FLOAT, { 0.2f, 0.4f, 0.6f, 1 });
    GDX12Texture localShadow;
    GDX12TextureDesc local;
    local.Width = local.Height = 1;
    local.Format = local.SRVDesc.Format = DXGI_FORMAT_R32_FLOAT;
    local.SRV_UAV_Heap = resources.SRV_UAV_Heap.get();
    local.SRVHeapIndex = local.SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength);
    local.SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    local.SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    local.SRVDesc.Texture2DArray.ArraySize = local.SRVDesc.Texture2DArray.MipLevels = 1;
    localShadow.Initialize(local);
    auto* queue = device.GetCommandQueue();
    auto* commands = queue->GetCommandList();
    commands->ResourceBarrier({ background->GetResource()->GetRenderTargetBarrier(), depth.GetResource()->GetDepthWriteBarrier() });
    commands->ClearRenderTargetView(background.get());
    commands->GetCommandList()->ClearDepthStencilView(depth.GetDSV()->CPUHandle, D3D12_CLEAR_FLAG_DEPTH,
        (10000.0f / 9000 - 1) / 9999, 0, 0, nullptr);
    queue->ExecuteCommandList(commands); queue->Flush();
    resources.FrameConstants[resources.CurrFrameConstantsIndex]->CameraVisibilityCommands.resize(1);

    GDX12FluidSurfacePass surface;
    GDX12FluidResolvePass resolve(&surface);
    using Partition = GDX12WBOITTransparencyPass::DepthPartition;
    GDX12WBOITTransparencyPass farTransparency(Partition::Far), nearTransparency(Partition::Near), allTransparency;
    GDX12ParticlesPass fullParticles, referenceParticles;
    GDX12WBOITCompositionPass behind, finalComposition, reference, noAerial;
    finalComposition.SetConditionalTransparency(&nearTransparency);
    finalComposition.SetApplyOpaqueAerialPerspective(false);
    finalComposition.SetParticles(&fullParticles);
    reference.SetParticles(&referenceParticles);
    noAerial.SetApplyOpaqueAerialPerspective(false);
    GDX12ParticleSimulationPass simulation;
    surface.SetInputs({ &depth, &motion });
    simulation.SetInputs({ &depth });
    const auto transparencyInputs = [&](GDX12WBOITTransparencyPass& pass, bool split)
    {
        std::vector<IRenderPassLink*> inputs{ &depth, &depth, environment.GetOutput(0),
            environment.GetOutput(1), environment.GetOutput(2), &localShadow };
        if (split) inputs.push_back(surface.GetOutput(0));
        pass.SetInputs(std::move(inputs));
    };
    transparencyInputs(farTransparency, true); transparencyInputs(nearTransparency, true); transparencyInputs(allTransparency, false);
    behind.SetInputs({ background.get(), farTransparency.GetOutput(0), farTransparency.GetOutput(1), &depth });
    resolve.SetInputs({ behind.GetOutput(0), environment.GetOutput(1) });
    fullParticles.SetInputs({ surface.GetOutput(0), nearTransparency.GetOutput(0), nearTransparency.GetOutput(1) });
    finalComposition.SetInputs({ resolve.GetOutput(0), nearTransparency.GetOutput(0), nearTransparency.GetOutput(1), surface.GetOutput(0) });
    referenceParticles.SetInputs({ &depth, allTransparency.GetOutput(0), allTransparency.GetOutput(1) });
    reference.SetInputs({ background.get(), allTransparency.GetOutput(0), allTransparency.GetOutput(1), &depth });
    noAerial.SetInputs({ background.get(), allTransparency.GetOutput(0), allTransparency.GetOutput(1), &depth });
    const std::vector<GDX12RenderPass*> passes{ &surface, &simulation, &farTransparency, &behind, &resolve, &nearTransparency,
        &fullParticles, &finalComposition, &allTransparency, &referenceParticles, &reference, &noAerial };
    for (auto* pass : passes) pass->Setup(&resources, nullptr, &common);
    GDX12RenderPass::InitializeAll(passes);
    WaterGraphOutput output;
    output.SetInputs({ finalComposition.GetOutput(0), reference.GetOutput(0), noAerial.GetOutput(0) });
    GDX12RenderGraph graph(&device);
    using Shared = GDX12RenderGraph::SharedResource;
    for (const auto resource : { Shared::Scene, Shared::Visibility, Shared::Environment, Shared::ReflectionProbes })
        graph.ImportShared(resource, "Water composition fixture");
    for (auto* input : std::vector<IRenderPassLink*>{ &depth, &motion, background.get(), &localShadow,
        environment.GetOutput(0), environment.GetOutput(1), environment.GetOutput(2) }) graph.ImportLink("Fixture texture", input);
    graph.AddPass(simulation); graph.AddPass(surface); graph.AddPass(farTransparency); graph.AddPass(behind);
    const auto water = graph.AddPass(resolve), front = graph.AddPass(nearTransparency);
    graph.DependsOn(front, water);
    graph.AddPass(fullParticles); graph.AddPass(finalComposition);
    graph.AddPass(allTransparency); graph.AddPass(referenceParticles); graph.AddPass(reference); graph.AddPass(noAerial); graph.AddPass(output);
    graph.Compile(false);

    GDX12ParticleEmitter emitter;
    emitter.Id = 887766; emitter.MaxParticles = 1; emitter.BurstTotal = 1; emitter.EmissionRate = 0;
    emitter.World = Matrix::CreateTranslation(eye + forward * 60);
    emitter.SpeedMin = emitter.SpeedMax = 0; emitter.Acceleration = Vector3::Zero;
    emitter.LifetimeMin = emitter.LifetimeMax = 10; emitter.DeltaTime = 0;
    emitter.SizeStart = emitter.SizeEnd = 20; emitter.SizeVariation = emitter.SoftDistance = 0;
    emitter.ColorStart = emitter.ColorEnd = Vector4(0.8f, 0.1f, 0.4f, 0.6f);
    GDX12FluidInstance disabled;
    disabled.Id = 887766; disabled.Component.SetParticles({ { Vector3::Zero, Vector3::Zero } });
    const auto render = [&]
    {
        surface.PrepareFrame(); resolve.PrepareFrame();
        resources.ParticleSimulation->BeginFrame(&resources, common);
        fullParticles.BeginFrame(); referenceParticles.BeginFrame();
        auto* list = queue->GetCommandList();
        resources.ParticleSimulation->InitializePools(list, &resources);
        list = graph.Execute(list);
        queue->ExecuteCommandList(list); queue->Flush();
        Require(surface.GetOutput(0)->GetTexture() == &depth && surface.GetOutput(1)->GetTexture() == &motion,
            "Off/empty composition changed opaque depth or motion identity");
        const auto actual = Read(device, *finalComposition.GetOutput(0)->GetTexture());
        Require(actual == Read(device, *reference.GetOutput(0)->GetTexture()),
            "Off/empty water HDR differs from ordinary transparency: particle/aerial duplicate or feedback");
        return actual;
    };
    Set(L"Render.Particles.Enabled", "true"); Set(L"Render.Particles.DrawEnabled", "true");
    Set(L"Render.Particles.SimulationEnabled", "true"); Set(L"Render.Fluid.Quality", "2");
    resources.Sky.Source = SkySource::Off; ++resources.Sky.Revision; execute(environment);
    const auto opaque = Read(device, *background);
    for (UINT state = 0; state < 3; ++state)
    {
        Set(L"Render.Fluid.Enabled", state == 2 ? "false" : "true");
        surface.SetInstances(state == 0 ? std::vector<GDX12FluidInstance>{} :
            std::vector<GDX12FluidInstance>{ state == 1 ? GDX12FluidInstance{} : disabled });
        for (const auto blend : { ParticleBlend::WeightedAlpha, ParticleBlend::SortedAlpha, ParticleBlend::Additive })
        {
            emitter.Blend = blend; ++emitter.ResetRevision; resources.ParticleEmitters = { emitter };
            Require(render() != opaque, "Single-particle water composition fixture produced no visible contribution");
        }
    }
    resources.Sky.Source = SkySource::SkyAtmosphere; ++resources.Sky.Revision; execute(environment);
    Set(L"Render.Particles.DrawEnabled", "false");
    for (bool disabledWater : { false, true })
    {
        Set(L"Render.Fluid.Enabled", disabledWater ? "false" : "true");
        surface.SetInstances(disabledWater ? std::vector<GDX12FluidInstance>{ disabled } : std::vector<GDX12FluidInstance>{});
        for (bool hasEmitter : { false, true })
        {
            resources.ParticleEmitters = hasEmitter ? std::vector<GDX12ParticleEmitter>{ emitter } : std::vector<GDX12ParticleEmitter>{};
            const auto actual = render();
            Require(actual != Read(device, *noAerial.GetOutput(0)->GetTexture()), "Atmosphere fixture did not produce measurable aerial perspective");
        }
    }
    resources.ParticleEmitters.clear();
    fullParticles.ClearDenendencies(); referenceParticles.ClearDenendencies();
    Set(L"Render.Particles.DrawEnabled", "true");
    resources.Sky.Source = oldSkySource; ++resources.Sky.Revision; execute(environment);
    std::cout << "Water Far/behind/resolve/Near/final vs ordinary route: three single-particle blend modes, absent/empty/off, aerial once passed.\n";
}
