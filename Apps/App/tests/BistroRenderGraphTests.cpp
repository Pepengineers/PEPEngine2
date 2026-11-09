#include "Rendering/BistroRenderPipeline.h"
#include "App.Base/App.h"
#include "../src/BistroScene.h"
#include "../src/BistroGrass.h"
#include "../src/Components/BistroInteractionSource.h"
#include "../src/BistroWater.h"
#include <charconv>
#include <d3d12sdklayers.h>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <chrono>
#include <fstream>
#include <map>

static void Set(const wchar_t* name, const char* value)
{
    if (!ConsoleModule{}.FindConsoleVariable(name).SetFromString(value)) throw std::runtime_error("Cannot change fixture setting: " + WStringToString(name));
}

static void CheckMessages(ID3D12InfoQueue& info)
{
    for (UINT64 i = 0; i < info.GetNumStoredMessages(); ++i)
    {
        SIZE_T size = 0;
        ThrowIfFailed(info.GetMessage(i, nullptr, &size));
        std::vector<BYTE> bytes(size);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
        ThrowIfFailed(info.GetMessage(i, message, &size));
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) throw std::runtime_error(message->pDescription);
    }
}

static std::vector<BYTE> CaptureHDR(GDX12Texture& texture)
{
    auto* resource = texture.GetResource()->D3DResource.Get();
    ComPtr<ID3D12Device> device;
    ThrowIfFailed(resource->GetDevice(IID_PPV_ARGS(&device)));
    const auto desc = resource->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT64 bytes;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
    const auto buffer = CD3DX12_RESOURCE_DESC::Buffer(bytes);
    ComPtr<ID3D12Resource> readback;
    ThrowIfFailed(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)));
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    ThrowIfFailed(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)));
    ThrowIfFailed(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
    ThrowIfFailed(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commands)));
    const D3D12_RESOURCE_BARRIER barrier = texture.GetResource()->GetCopySourceBarrier();
    if (barrier.Transition.StateBefore != barrier.Transition.StateAfter) commands->ResourceBarrier(1, &barrier);
    const CD3DX12_TEXTURE_COPY_LOCATION source(resource, 0), destination(readback.Get(), footprint);
    commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    ThrowIfFailed(commands->Close());
    ID3D12CommandList* lists[] = { commands.Get() };
    queue->ExecuteCommandLists(1, lists);
    ComPtr<ID3D12Fence> fence;
    ThrowIfFailed(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    ThrowIfFailed(queue->Signal(fence.Get(), 1));
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) throw std::runtime_error("Cannot create readback fence event");
    const auto result = fence->SetEventOnCompletion(1, event);
    const auto wait = SUCCEEDED(result) ? WaitForSingleObject(event, 30000) : WAIT_FAILED;
    CloseHandle(event);
    ThrowIfFailed(result);
    if (wait != WAIT_OBJECT_0) throw std::runtime_error("Bistro readback timed out");
    void* mapped;
    ThrowIfFailed(readback->Map(0, nullptr, &mapped));
    std::vector<BYTE> pixels(size_t(texture.GetWidth()) * texture.GetHeight() * 8);
    for (UINT y = 0; y < texture.GetHeight(); ++y)
        std::memcpy(pixels.data() + size_t(y) * texture.GetWidth() * 8,
            static_cast<const BYTE*>(mapped) + footprint.Offset + y * footprint.Footprint.RowPitch,
            size_t(texture.GetWidth()) * 8);
    readback->Unmap(0, nullptr);
    return pixels;
}

class BistroRenderGraphTests final : public App
{
public:
    BistroRenderGraphTests() : App(GetModuleHandleW(nullptr)) {}
    void Frame(double step = -1, double elapsed = -1)
    {
        MSG message;
        while (PeekMessage(&message, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&message);
            DispatchMessage(&message);
        }
        auto render = Locator.GetModule<RenderModule>();
        auto& world = *Locator.GetModule<SceneManagerModule>()->GetWorld();
        if (elapsed >= 0) Timer.Tick(step, elapsed); else Timer.Tick(step);
        render->BeginFrame();
        render->Update();
        auto work = _lights.Schedule(_jobs, world, step >= 0 ? static_cast<float>(step) : 0);
        _gpuData.Schedule(_jobs, world, step >= 0 ? static_cast<float>(step) : 0, work).Complete();
        render->Render();
    }

#ifdef _DEBUG
    void CheckCollisionView()
    {
        Timer.Reset();
        Timer.Stop();
        auto render = Locator.GetModule<RenderModule>();
        const auto scenePasses = render->GetRenderGraphStatistics().Passes;
        auto* output = render->GetRenderPipelineCommonData()->Upscaler->GetOutput(0)->GetTexture();
        ComPtr<ID3D12Device> device;
        ThrowIfFailed(output->GetResource()->D3DResource->GetDevice(IID_PPV_ARGS(&device)));
        ComPtr<ID3D12InfoQueue> info;
        ThrowIfFailed(device.As(&info));
        info->ClearStoredMessages();
        for (bool enabled : {true, false, true, false})
        {
            BistroRenderPipeline::ToggleCollisionView();
            for (int frame = 0; frame < 3; ++frame) Frame();
            render->WaitForUploads();
            CheckMessages(*info.Get());
            if (render->GetRenderGraphStatistics().Passes != (enabled ? 1u : scenePasses) ||
                render->GetFrameGenerationStatus().Suspended != enabled)
                throw std::runtime_error("Collision view did not switch the scene graph and frame generation together");
        }
        std::cout << "Collision-only graph, frame generation suspension and return to the scene passed\n";
    }
#endif

    void Check()
    {
        Timer.Reset();
        Timer.Stop();
        auto render = Locator.GetModule<RenderModule>();
        auto& world = *Locator.GetModule<SceneManagerModule>()->GetWorld();
        BistroGrass::Scene grass;
        grass.Update(world, 0);
        auto& camera = world.GetECS().Get<TransformComponent>(world.ActiveCamera);
        camera.Location = Vector3(-300, 200, 20);
        camera.Rotation = Vector3(0, 117, 0);
        camera.DirtyFlag = true;
        world.GetECS().Get<CameraComponent>(world.ActiveCamera).InvalidateHistory();
        Set(L"Render.Upscaler.Provider", "0");
        Set(L"Render.SSR.Enabled", "false");
        Set(L"Render.DOF.Enabled", "false");
        Set(L"Render.MotionBlur.Enabled", "false");
        Set(L"Render.Reflection.Enabled", "false");
        Set(L"Render.Particles.Enabled", "true");
        // Atomic particle compaction changes FP16 blend order even with a zero simulation delta.
        Set(L"Render.Particles.DrawEnabled", "false");
        // Atomic culling reorders transparency and the asset's coplanar ceramic/plate-details overlays.
        std::map<GDX12Material*, std::pair<MaterialType, float>> materialStates;
        bool plateOverlayFound = false;
        world.GetECS().ForEach<StaticMeshRenderComponent>([&](Entity, StaticMeshRenderComponent& mesh)
        {
            const auto& imported = render->GetPrimaryGPUMesh(mesh.MeshHandler)->CPUMesh->GetMaterials();
            for (size_t slot = 0; slot < mesh.Materials.size(); ++slot)
            {
                auto* material = mesh.Materials[slot];
                const bool plateOverlay = material && material->Type == MaterialType::Masked &&
                    slot < imported.size() && imported[slot].Name == "Plates_Details";
                plateOverlayFound |= plateOverlay;
                if (material && (material->Type == MaterialType::Transparent || plateOverlay))
                {
                    materialStates.try_emplace(material, material->Type, material->Opacity);
                    material->Type = MaterialType::Transparent;
                    material->Opacity = 0;
                    material->DirtyFlag = true;
                }
            }
        });
        if (!plateOverlayFound) throw std::runtime_error("Fixture did not find the coplanar plate overlay");
        Set(L"Render.AO.Implementation", "0");
        Set(L"Render.AO.Enabled", "0");
        Frame();
        render->WaitForUploads();
        auto* output = render->GetRenderPipelineCommonData()->Upscaler->GetOutput(0)->GetTexture();
        ComPtr<ID3D12Device> device;
        ThrowIfFailed(output->GetResource()->D3DResource->GetDevice(IID_PPV_ARGS(&device)));
        ComPtr<ID3D12InfoQueue> info;
        ThrowIfFailed(device.As(&info));
        info->ClearStoredMessages();
        for (int early : { 0, 1 })
        {
            Set(L"Render.AO.ExecutionPoint", early ? "0" : "1");
            std::vector<BYTE> reference;
            for (bool reuse : { false, false, true, false })
            {
                for (bool async : { false, false, true, false })
                {
                    Set(L"Render.Particles.AsyncCompute", async ? "true" : "false");
                    Set(L"Render.Graph.ReuseTextures", reuse ? "true" : "false");
                    for (int frame = 0; frame < 4; ++frame) Frame();
                    render->WaitForUploads();
                    CheckMessages(*info.Get());
                    output = render->GetRenderPipelineCommonData()->Upscaler->GetOutput(0)->GetTexture();
                    const auto pixels = CaptureHDR(*output);
                    if (reference.empty()) reference = pixels;
                    else if (pixels != reference) throw std::runtime_error("Bistro HDR differs from the first dedicated capture (reuse=" +
                        std::to_string(reuse) + ", async=" + std::to_string(async) + ")");
                    const auto stats = render->GetRenderGraphStatistics();
                    if (stats.Passes > 64) throw std::runtime_error("Bistro graph expanded geometry views into individual passes");
                    if (reuse && stats.PhysicalTextures >= stats.TransientTextures) throw std::runtime_error("Bistro graph did not reuse textures");
                    if (!reuse && stats.PhysicalTextures != stats.TransientTextures) throw std::runtime_error("Dedicated graph mode still shares textures");
                    if (stats.AsyncPasses != (async ? 1u : 0u)) throw std::runtime_error("Bistro particle queue selection failed");
                    if (async && stats.JoinBefore != "Fluid surface")
                        throw std::runtime_error("Bistro joined before its first declared depth hazard: " + stats.JoinBefore);
                    std::cout << "Bistro earlyAO=" << early << " reuse=" << reuse << " async=" << async << " passes=" << stats.Passes << " textures="
                        << stats.PhysicalTextures << '/' << stats.TransientTextures << " join=" << stats.JoinBefore
                        << " HDR readback identical" << std::endl;
                }
            }
            const auto directory = std::filesystem::path(SOLUTION_DIR) / "Build/Verification";
            std::filesystem::create_directories(directory);
            std::ofstream graph(directory / (early ? "bistro-early-ao.dot" : "bistro-late-ao.dot"));
            render->WriteRenderGraph(graph);
            if (!graph) throw std::runtime_error("Cannot write Bistro render graph");
        }
        Set(L"Render.Graph.ReuseTextures", "true");
        Set(L"Render.AO.Enabled", "1");
        Set(L"Render.Particles.AsyncCompute", "true");
        for (int early : { 0, 1 })
            for (int implementation = 0; implementation < 4; ++implementation)
            {
                Set(L"Render.AO.ExecutionPoint", early ? "0" : "1");
                Set(L"Render.AO.Implementation", std::to_string(implementation).c_str());
                for (int frame = 0; frame < 4; ++frame) Frame();
                render->WaitForUploads();
                CheckMessages(*info.Get());
                if (render->GetRenderGraphStatistics().JoinBefore != "Fluid surface")
                    throw std::runtime_error("Active AO joined its shared depth reader early: " + render->GetRenderGraphStatistics().JoinBefore);
                std::cout << "Bistro shared depth: active AO=" << implementation << " early=" << early << " passed." << std::endl;
            }
        Set(L"Render.SSR.Enabled", "true");
        Set(L"Render.DOF.Enabled", "true");
        Set(L"Render.MotionBlur.Enabled", "true");
        Set(L"Render.Reflection.Enabled", "true");
        Set(L"Render.Particles.AsyncCompute", "true");
        std::cout << "Bistro async: all effects and odd resize started." << std::endl;
        Set(L"Render.Particles.DrawEnabled", "true");
        for (const auto& [material, state] : materialStates)
        {
            material->Type = state.first;
            material->Opacity = state.second;
            material->DirtyFlag = true;
        }
        SetWindowPos(GetWindow()->GetWindowHandle(), nullptr, 0, 0, 973, 657, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        for (int frame = 0; frame < 4; ++frame) Frame();
        render->WaitForUploads();
        CheckMessages(*info.Get());
        std::cout << "Bistro async: odd resize passed; Native/XeSS switch started." << std::endl;
        Set(L"Render.Upscaler.Provider", "1");
        for (int frame = 0; frame < 4; ++frame) Frame();
        render->WaitForUploads();
        Set(L"Render.Particles.AsyncCompute", "false");
        CheckMessages(*info.Get());
        std::cout << "Bistro graph: pooled/dedicated HDR identity, both AO placements, odd resize and native/XeSS switch passed." << std::endl;
    }
    void GrassExperiment(bool timing)
    {
        Timer.Reset(); Timer.Start();
        auto render = Locator.GetModule<RenderModule>();
        auto& world = *Locator.GetModule<SceneManagerModule>()->GetWorld();
        auto& ecs = world.GetECS();
        ecs.RegisterComponent<BistroInteractionSource>();
        std::vector<Entity> sources;
        for (UINT i = 0; i < 32; ++i)
        {
            auto entity = ecs.CreateEntity();
            entity.AddComponent<BistroInteractionSource>(); sources.push_back(entity.GetId());
        }
        BistroGrass::Scene grass; grass.Update(world, 0);
        Set(L"Render.FrameGeneration.Provider", "0"); Set(L"Render.Upscaler.Provider", "0");
        Set(L"Render.AO.ExecutionPoint", "1");
        Set(L"App.Particles.Count", "10000"); BistroParticles::Update(world);
        auto& camera = ecs.Get<TransformComponent>(world.ActiveCamera);
        camera.Location = BistroArch::GrassPosition + Vector3(450,200,-450);
        auto direction = BistroArch::GrassPosition + Vector3(0,70,0) - camera.Location; direction.Normalize();
        camera.Rotation = Vector3(XMConvertToDegrees(-std::asin(direction.y)), XMConvertToDegrees(std::atan2(direction.x,direction.z)),0);
        camera.DirtyFlag = true;
        ComPtr<ID3D12InfoQueue> info;
        if (!timing)
        {
            ComPtr<ID3D12Device> device;
            ThrowIfFailed(render->GetRenderPipelineCommonData()->Upscaler->GetOutput(0)->GetTexture()->GetResource()->D3DResource->GetDevice(IID_PPV_ARGS(&device)));
            ThrowIfFailed(device.As(&info)); info->ClearStoredMessages();
        }
        std::filesystem::create_directories("Build/GrassInteraction");
        std::ofstream samples("Build/GrassInteraction/bistro-timing.csv");
        samples << "wind,series,async,sample,sources,gpu_ms,compute_sum_ms,cpu_submit_ms,drain\n";
        BistroWater::Scene water;
        for (int wind : {0,1})
        {
            Set(L"App.Grass.Wind.Algorithm", wind ? "1" : "0"); grass.Update(world, 0);
            int series = 0;
            for (bool async : {false,true,true,false})
            {
                Set(L"Render.Grass.AsyncCompute", async ? "true" : "false");
                Set(L"Render.Fluid.AsyncCompute", !timing && async ? "true" : "false");
                Set(L"Render.Particles.AsyncCompute", !timing && async ? "true" : "false");
                Set(L"Render.AO.AsyncCompute", !timing && async ? "true" : "false");
                Set(L"App.Water.Count", timing ? "0" : "1"); water.Update(world, 1.0f/60);
                ecs.ForEach<ParticleEmitterComponent>([](Entity, ParticleEmitterComponent& emitter)
                { emitter.Reset(); if (emitter.IsEnabled()) emitter.Emit(emitter.GetSettings().MaxParticles); });
                ecs.ForEach<WindFieldComponent>([](Entity, WindFieldComponent& field) { field.Reset(); });
                ecs.ForEach<GrassRenderComponent>([](Entity, GrassRenderComponent& patch) { patch.ResetHistory(); });
                const int warmup = timing ? 16 : 4, count = timing ? 60 : 8;
                for (int frame = 0; frame < warmup + count; ++frame)
                {
                    for (UINT i = 0; i < sources.size(); ++i)
                    {
                        auto& source = ecs.Get<BistroInteractionSource>(sources[i]);
                        const float x = BistroArch::GrassPosition.x + float(i % 8) * 35 - 120;
                        const float z = BistroArch::GrassPosition.z + std::sin((frame + i) * 0.05f) * 180;
                        source.PreviousHeight = frame ? XMFLOAT4(source.PositionRadius.x, source.PositionRadius.y, source.PositionRadius.z,190) : XMFLOAT4(x,23.3f,z,190);
                        source.PositionRadius = XMFLOAT4(x,23.3f,z,65);
                    }
                    water.Update(world, 1.0f/60);
                    const auto start = std::chrono::steady_clock::now(); Frame(1.0/60,(frame+1)/60.0);
                    const double cpu = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
                    if (timing) render->WaitForUploads(); // Explicit CPU drain: not pipelined FPS.
                    if (frame >= warmup)
                        samples << wind << ',' << series << ',' << async << ',' << frame-warmup << ",32," << render->GetGPUFrameTimeMS()
                            << ',' << render->GetRenderGraphStatistics().ComputeTimeMS << ',' << cpu << ',' << timing << '\n';
                }
                render->WaitForUploads();
                if (info) CheckMessages(*info.Get());
                std::ofstream graph("Build/GrassInteraction/graph-w" + std::to_string(wind) + "-s" + std::to_string(series) + ".dot");
                render->WriteRenderGraph(graph);
                if (async && render->GetRenderGraphStatistics().AsyncPasses < 1) throw std::runtime_error("Grass COMPUTE dispatch missing");
                std::cout << "Grass experiment wind=" << wind << " series=" << series++ << " async=" << async << " passed; CPU-drain=" << timing << std::endl;
            }
        }
        if (!timing)
        {
            Set(L"Render.Grass.AsyncCompute", "true"); Set(L"Render.Fluid.AsyncCompute", "true");
            Set(L"Render.Particles.AsyncCompute", "true"); Set(L"Render.AO.AsyncCompute", "true");
            for (int scenario = 0; scenario < 4; ++scenario)
            {
                Set(L"Render.Wind.Enabled", scenario == 0 ? "false" : "true");
                Set(L"Render.Grass.Enabled", scenario == 1 ? "false" : "true");
                Set(L"Render.Graph.ReuseTextures", scenario & 1 ? "true" : "false");
                if (scenario == 2) ecs.ForEach<GrassRenderComponent>([](Entity, GrassRenderComponent& patch) { ++patch.Seed; patch.ResetHistory(); });
                SetWindowPos(GetWindow()->GetWindowHandle(),nullptr,0,0,973+scenario,657,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
                world.SetPaused(scenario == 3);
                for (int frame=0;frame<4;++frame) Frame(scenario == 3 ? 0 : 1.0/60,(frame+1)/60.0);
                render->WaitForUploads(); CheckMessages(*info.Get());
            }
            world.SetPaused(false);
            std::cout << "Grass combined async: queued frames, water/particles/AO, disabled wind/grass, seed reset, pause and odd resize passed." << std::endl;
        }
    }
    void Time()
    {
        Timer.Reset(); Timer.Stop();
        auto render = Locator.GetModule<RenderModule>();
        auto& world = *Locator.GetModule<SceneManagerModule>()->GetWorld();
        Set(L"Render.FrameGeneration.Provider", "0");
        Set(L"Render.Upscaler.Provider", "0");
        Set(L"Render.Particles.Enabled", "true");
        Set(L"Render.Particles.DrawEnabled", "true");
        Set(L"Render.Particles.SimulationEnabled", "true");
        Set(L"App.Particles.Count", "250000");
        BistroParticles::Update(world);
        std::ofstream samples("Build/async-bistro-timing.csv");
        if (!samples) throw std::runtime_error("Cannot create timing report");
        samples << "series,async,sample,gpu_ms,compute_ms,cpu_submit_ms\n";
        int series = 0;
        for (bool async : {false, true, false})
        {
            Set(L"Render.Particles.AsyncCompute", async ? "true" : "false");
            world.GetECS().ForEach<ParticleEmitterComponent>([](Entity, ParticleEmitterComponent& emitter)
            {
                emitter.Reset();
                emitter.Emit(emitter.GetSettings().MaxParticles);
            });
            for (int frame = 0; frame < 24; ++frame) Frame();
            render->WaitForUploads();
            std::cout << "Timing series=" << series << " async=" << async << " 250000 particles, frozen dt=0" << std::endl;
            for (int frame = 0; frame < 120; ++frame)
            {
                const auto started = std::chrono::steady_clock::now();
                Frame();
                const double cpu = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
                render->WaitForUploads();
                const auto& stats = render->GetRenderGraphStatistics();
                samples << series << ',' << async << ',' << frame << ',' << render->GetGPUFrameTimeMS() << ','
                    << (async ? stats.ComputeTimeMS : -1) << ',' << cpu << '\n';
            }
            std::ofstream graph("Build/async-bistro-series-" + std::to_string(series) + ".dot");
            render->WriteRenderGraph(graph);
            ++series;
        }
        Set(L"Render.Particles.AsyncCompute", "false");
        if (!samples) throw std::runtime_error("Cannot write timing report");
    }
    void CheckAOPlacements()
    {
        Timer.Reset(); Timer.Stop();
        Set(L"Render.Fluid.AsyncCompute", "false"); Set(L"Render.Particles.AsyncCompute", "false");
        auto render = Locator.GetModule<RenderModule>();
        ComPtr<ID3D12Device> device;
        auto* output = render->GetRenderPipelineCommonData()->Upscaler->GetOutput(0)->GetTexture();
        ThrowIfFailed(output->GetResource()->D3DResource->GetDevice(IID_PPV_ARGS(&device)));
        ComPtr<ID3D12InfoQueue> info; ThrowIfFailed(device.As(&info)); info->ClearStoredMessages();
        for (int early : {0, 1})
            for (int implementation = 0; implementation < 4; ++implementation)
                for (bool async : {false, true, false})
                {
                    Set(L"Render.AO.ExecutionPoint", early ? "0" : "1");
                    Set(L"Render.AO.Implementation", std::to_string(implementation).c_str());
                    Set(L"Render.AO.AsyncCompute", async ? "true" : "false");
                    for (int frame = 0; frame < 2; ++frame) Frame();
                    render->WaitForUploads(); CheckMessages(*info.Get());
                    if (render->GetRenderGraphStatistics().AsyncPasses != (async ? 1u : 0u))
                        throw std::runtime_error("AO placement queue selection failed");
                    std::cout << "AO placement early=" << early << " implementation=" << implementation << " async=" << async << " passed." << std::endl;
                }
        Set(L"Render.AO.ExecutionPoint", "1"); Set(L"Render.AO.Implementation", "2"); Set(L"Render.AO.AsyncCompute", "false");
    }
    void Matrix(int argc, char** argv, bool validation)
    {
        int row = -1, warmup = 24, measured = 120;
        bool withAO = false;
        for (int i = 2; i < argc; ++i)
        {
            const std::string_view option(argv[i]);
            if (option == "--matrix-ao") { withAO = true; continue; }
            const auto parse = [&](std::string_view prefix, int& value)
            {
                if (option.substr(0, prefix.size()) != prefix) return false;
                const auto digits = option.substr(prefix.size());
                const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
                if (error != std::errc{} || end != digits.data() + digits.size() || value < 0)
                    throw std::invalid_argument("Invalid matrix integer");
                return true;
            };
            if (!parse("--row=", row) && !parse("--warmup=", warmup) && !parse("--samples=", measured))
                throw std::invalid_argument("Unknown matrix option");
        }
        const int combinations = withAO ? 8 : 4;
        if (row >= combinations || measured == 0 || warmup > 100000 || measured > 1000000) throw std::invalid_argument("Invalid matrix row or sample count");
        Set(L"Render.AO.AsyncCompute", "false");
        if (withAO)
        {
            Set(L"Render.AO.Enabled", "true"); Set(L"Render.AO.Implementation", "2"); Set(L"Render.AO.Quality", "2");
        }
        Timer.Reset(); Timer.Start();
        auto render = Locator.GetModule<RenderModule>();
        auto& world = *Locator.GetModule<SceneManagerModule>()->GetWorld();
        Set(L"Render.FrameGeneration.Provider", "0"); Set(L"Render.Upscaler.Provider", "0");
        Set(L"Render.AO.ExecutionPoint", "1");
        Set(L"Render.Fluid.Enabled", "true"); Set(L"Render.Fluid.Quality", "2"); Set(L"App.Water.Count", "1");
        Set(L"Render.Particles.Enabled", "true"); Set(L"Render.Particles.DrawEnabled", "true");
        Set(L"Render.Particles.SimulationEnabled", "true"); Set(L"App.Particles.Count", "250000");
        BistroGrass::Scene grass; grass.Update(world, 0);
        BistroWater::Scene water; water.Update(world, 1.0f / 60);
        BistroParticles::Update(world);
        auto& camera = world.GetECS().Get<TransformComponent>(world.ActiveCamera);
        camera.Location = BistroWater::Center + Vector3(420,330,420);
        auto direction = BistroWater::Center - camera.Location; direction.Normalize();
        camera.Rotation = Vector3(XMConvertToDegrees(-std::asin(direction.y)),
            XMConvertToDegrees(std::atan2(direction.x, direction.z)), 0);
        camera.DirtyFlag = true;
        UINT waterInstances = 0; UINT64 waterParticles = 0, particleCapacity = 0;
        world.GetECS().ForEach<FluidRenderComponent>([&](Entity, FluidRenderComponent& fluid)
        {
            auto settings = fluid.GetSettings(); settings.RenderEnabled = settings.SimulationEnabled = true;
            fluid.SetSettings(settings);
            ++waterInstances;
            if (fluid.GetParticles()) waterParticles += fluid.GetParticles()->size();
        });
        world.GetECS().ForEach<ParticleEmitterComponent>([&](Entity, ParticleEmitterComponent& emitter)
        { if (emitter.IsEnabled()) particleCapacity += emitter.GetSettings().MaxParticles; });
        if (waterInstances != 1 || waterParticles != 51975 || particleCapacity != 250000)
            throw std::runtime_error("Matrix ECS water/particle population does not match the frozen workload");
        ComPtr<ID3D12InfoQueue> info;
        if (validation)
        {
            ComPtr<ID3D12Device> device;
            auto* output = render->GetRenderPipelineCommonData()->Upscaler->GetOutput(0)->GetTexture();
            ThrowIfFailed(output->GetResource()->D3DResource->GetDevice(IID_PPV_ARGS(&device)));
            ThrowIfFailed(device.As(&info)); info->ClearStoredMessages();
        }
        std::filesystem::create_directories("Build/AsyncWaterMatrix");
        std::ofstream samples("Build/AsyncWaterMatrix/samples.csv");
        if (!samples) throw std::runtime_error("Cannot create matrix CSV");
        samples << "series,order,row,water_async,particles_async,ao_async,sample,dt,gpu_ms,compute_sum_ms,cpu_submit_ms,water_instances,water_particles,particle_capacity,drain\n";
        std::vector<int> rows;
        if (row >= 0) rows.push_back(row);
        else
        {
            for (int i = 0; i < combinations; ++i) rows.push_back(i);
            for (int i = combinations - 1; i >= 0; --i) rows.push_back(i);
        }
        for (size_t series = 0; series < rows.size(); ++series)
        {
            const int mask = rows[series];
            render->WaitForUploads();
            Timer.Reset(); Timer.Start();
            world.GetECS().ForEach<WindFieldComponent>([](Entity, WindFieldComponent& wind) { wind.Reset(); });
            world.GetECS().ForEach<GrassRenderComponent>([](Entity, GrassRenderComponent& patch) { patch.ResetHistory(); });
            Set(L"Render.Fluid.AsyncCompute", mask & 1 ? "true" : "false");
            Set(L"Render.Particles.AsyncCompute", mask & 2 ? "true" : "false");
            if (withAO) Set(L"Render.AO.AsyncCompute", mask & 4 ? "true" : "false");
            world.GetECS().ForEach<FluidRenderComponent>([](Entity, FluidRenderComponent& fluid) { fluid.Reset(); });
            world.GetECS().ForEach<ParticleEmitterComponent>([](Entity, ParticleEmitterComponent& emitter)
            { emitter.Reset(); if (emitter.IsEnabled()) emitter.Emit(emitter.GetSettings().MaxParticles); });
            world.GetECS().Get<CameraComponent>(world.ActiveCamera).InvalidateHistory();
            render->GetRenderPipelineCommonData()->ResetHistory = true;
            // Reset frames warm histories identically; measured CPU time excludes the explicit drain.
            for (int frame = 0; frame < warmup; ++frame) { water.Update(world, 1.0f / 60); Frame(1.0 / 60, (frame + 1) / 60.0); }
            render->WaitForUploads();
            std::cout << "Matrix series=" << series << " row=" << mask << " dt=1/60 water=" << waterParticles
                << " particle_capacity=" << particleCapacity << " CPU-drain" << std::endl;
            for (int frame = 0; frame < measured; ++frame)
            {
                water.Update(world, 1.0f / 60);
                const auto started = std::chrono::steady_clock::now(); Frame(1.0 / 60, (warmup + frame + 1) / 60.0);
                const double cpu = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
                render->WaitForUploads();
                if (info) CheckMessages(*info.Get());
                const auto& stats = render->GetRenderGraphStatistics();
                const UINT expected = !!(mask & 1) + !!(mask & 2) + !!(mask & 4);
                if (stats.AsyncPasses != expected) throw std::runtime_error("Independent matrix queue selection failed");
                if (validation && withAO && (mask & 4) && stats.JoinBefore != "Clustered deferred lighting")
                    throw std::runtime_error("Late AO joined before lighting: " + stats.JoinBefore);
                samples << series << ',' << (row >= 0 ? "single" : (series < size_t(combinations) ? "forward" : "reverse")) << ','
                    << mask << ',' << !!(mask & 1) << ',' << !!(mask & 2) << ',' << !!(mask & 4) << ',' << frame
                    << ",0.016666666666666667," << render->GetGPUFrameTimeMS() << ',' << stats.ComputeTimeMS << ',' << cpu
                    << ',' << waterInstances << ',' << waterParticles << ',' << particleCapacity << ",cpu-drain\n";
            }
            std::ofstream dot("Build/AsyncWaterMatrix/series-" + std::to_string(series) + ".dot"); render->WriteRenderGraph(dot);
            if (!dot) throw std::runtime_error("Cannot write matrix graph");
        }
        if (validation)
        {
            Set(L"Render.Fluid.AsyncCompute", "true"); Set(L"Render.Particles.AsyncCompute", "true");
            if (withAO) Set(L"Render.AO.AsyncCompute", "true");
            for (bool reuse : {false, true})
            {
                Set(L"Render.Graph.ReuseTextures", reuse ? "true" : "false");
                for (int frame = 0; frame < 3; ++frame) Frame(1.0 / 60, (warmup + measured + frame + 1) / 60.0);
                render->WaitForUploads(); CheckMessages(*info.Get());
            }
            SetWindowPos(GetWindow()->GetWindowHandle(), nullptr, 0, 0, 973, 657, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            for (int frame = 0; frame < 3; ++frame) Frame(1.0 / 60, (warmup + measured + frame + 4) / 60.0);
            render->WaitForUploads(); CheckMessages(*info.Get());
            Set(L"Render.Upscaler.Provider", "1");
            for (int frame = 0; frame < 3; ++frame) Frame(1.0 / 60, (warmup + measured + frame + 7) / 60.0);
            render->WaitForUploads(); CheckMessages(*info.Get());
            std::cout << "Matrix combined water/particles: pooling, queued frames, odd resize and Native/XeSS passed." << std::endl;
        }
        if (!samples) throw std::runtime_error("Cannot write matrix samples");
        Set(L"Render.Fluid.AsyncCompute", "false"); Set(L"Render.Particles.AsyncCompute", "false");
        if (withAO) Set(L"Render.AO.AsyncCompute", "false");
    }
protected:
    bool AddModules() override
    {
        auto scenes = std::make_shared<SceneManagerModule>(std::filesystem::path(ASSETS_FOLDER) / "Scenes/Amazon Lumberyard Bistro");
        auto renderer = std::make_shared<RenderModule>(GetWindow(), &Timer,
            std::make_unique<BistroRenderPipeline>(scenes));
        Locator.RegisterModule(renderer);
        ConfigureBistroProps(*scenes, GetJobs());
        Locator.RegisterModule(scenes);
        Locator.RegisterModule(std::make_shared<PhysicsModule>(_jobs));
        return BenchmarkEngine::AddModules();
    }
};

int main(int argc, char** argv)
{
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    try
    {
        const std::string_view mode = argc > 1 ? argv[1] : "";
        const bool matrix = mode == "--matrix" || mode == "--matrix-validation" || mode == "--matrix-gpu-validation";
        const bool timing = mode == "--timing" || mode == "--grass-timing" || matrix;
        const bool validationMode = mode == "--matrix-validation" || mode == "--matrix-gpu-validation";
        std::cout.setf(std::ios::unitbuf);
        ComPtr<ID3D12Debug> debug;
        if (!timing || validationMode)
        {
            ThrowIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
            debug->EnableDebugLayer();
        }
        if (mode == "--gpu-validation" || mode == "--matrix-gpu-validation" || mode == "--ao-placements-gpu-validation" || mode == "--grass-gpu-validation" || mode == "--collision-view-gpu-validation")
        {
            ComPtr<ID3D12Debug1> validation;
            ThrowIfFailed(debug.As(&validation));
            validation->SetEnableGPUBasedValidation(TRUE);
        }
        GameTimer fixedClock; fixedClock.Reset(); fixedClock.Tick(1.0 / 60, 2.0);
        if (std::abs(fixedClock.DeltaTime() - 1.0f / 60) > 1e-7f || std::abs(fixedClock.TotalTime() - 2.0f) > 1e-6f)
            throw std::runtime_error("Deterministic matrix clock self-check failed");
        fixedClock.Stop(); fixedClock.Tick(1.0 / 60, 3.0);
        if (fixedClock.DeltaTime() != 0) throw std::runtime_error("Paused fixed clock advanced");
        BistroRenderGraphTests app;
        if (!app.Initialize()) throw std::runtime_error("Bistro initialization failed");
#ifdef _DEBUG
        if (mode == "--collision-view" || mode == "--collision-view-gpu-validation") app.CheckCollisionView();
        else
#endif
        if (mode == "--ao-placements" || mode == "--ao-placements-gpu-validation") app.CheckAOPlacements();
        else if (mode == "--grass-timing" || mode == "--grass-validation" || mode == "--grass-gpu-validation") app.GrassExperiment(timing);
        else if (matrix) app.Matrix(argc, argv, validationMode);
        else if (timing) app.Time();
        else app.Check();
        return 0;
    }
    catch (const DxException& error) { std::wcerr << error.ToString() << '\n'; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
    return 1;
}
