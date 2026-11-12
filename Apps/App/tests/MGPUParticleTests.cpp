#include "App.Base/App.h"
#include "App.Base/Rendering/CrossAdapterParticles.h"
#include "Engine.RendererDX12/GDX12DeviceFactory.h"
#include <d3d12sdklayers.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <string_view>

namespace
{
using Graph = GDX12RenderGraph;
class TestApp final : public App { public: TestApp() : App(GetModuleHandleW(nullptr)) {} };
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void Set(const wchar_t* name, const char* value)
{
    Check(ConsoleModule{}.FindConsoleVariable(name).SetFromString(value), "Cannot set particle test control");
}
ComPtr<IDXGIAdapter4> OtherAdapter(LUID primary)
{
    for (UINT index = 0;; ++index)
    {
        ComPtr<IDXGIAdapter4> adapter;
        const auto status = GDX12DeviceFactory::GetFactory()->EnumAdapterByGpuPreference(index,
            DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
        if (status == DXGI_ERROR_NOT_FOUND) break;
        ThrowIfFailed(status);
        DXGI_ADAPTER_DESC1 desc{};
        ThrowIfFailed(adapter->GetDesc1(&desc));
        if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
            (desc.AdapterLuid.HighPart != primary.HighPart || desc.AdapterLuid.LowPart != primary.LowPart) &&
            SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device14), nullptr)))
            return adapter;
    }
    throw std::runtime_error("Particle MGPU tests require two physical D3D12 adapters");
}
void CheckDebug(GDX12Device& device)
{
    ThrowIfFailed(device.GetDevice()->GetDeviceRemovedReason());
    ComPtr<ID3D12InfoQueue> info;
    ThrowIfFailed(device.GetDevice().As(&info));
    for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i)
    {
        SIZE_T size = 0;
        ThrowIfFailed(info->GetMessage(i, nullptr, &size));
        std::vector<BYTE> storage(size);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        ThrowIfFailed(info->GetMessage(i, message, &size));
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) throw std::runtime_error(message->pDescription);
    }
}
std::vector<std::byte> Read(GDX12Device& device, GDX12Resource& source)
{
    const auto size = source.D3DResource->GetDesc().Width;
    auto readback = device.CreateResource(D3D12_HEAP_TYPE_READBACK, CD3DX12_RESOURCE_DESC::Buffer(size),
        D3D12_RESOURCE_STATE_COPY_DEST);
    auto& queue = *device.GetCommandQueue();
    auto* commands = queue.GetCommandList();
    const auto before = source.GetCurrentState();
    commands->ResourceBarrier({source.GetCopySourceBarrier()});
    commands->GetCommandList()->CopyBufferRegion(readback.Get(), 0, source.D3DResource.Get(), 0, size);
    commands->ResourceBarrier({source.GetTransitionBarrier(before)});
    queue.ExecuteCommandList(commands);
    queue.Flush();
    const D3D12_RANGE read{0, static_cast<SIZE_T>(size)}, noWrite{};
    void* mapped = nullptr;
    ThrowIfFailed(readback->Map(0, &read, &mapped));
    std::vector<std::byte> result(static_cast<size_t>(size));
    std::memcpy(result.data(), mapped, result.size());
    readback->Unmap(0, &noWrite);
    return result;
}
struct Particle
{
    XMFLOAT3 Position; float Age;
    XMFLOAT3 Previous; float Lifetime;
    XMFLOAT3 Velocity; float Rotation;
    float PreviousAge, PreviousRotation, SizeRandom; UINT Seed;
};
static_assert(sizeof(Particle) == 64);
struct Result { std::vector<Particle> Particles; UINT Alive; CrossAdapterParticles::Diagnostics Status; };

Result Run(GDX12DeviceResources& resources, GDX12Device& secondary, RenderMGPURuntime& runtime,
    bool secondaryPlacement, UINT cadence, bool exact, bool changingInputs, bool switchOwner, bool lifecycle = false)
{
    std::cout << "Particle case: secondary=" << secondaryPlacement << " cadence=" << cadence << " exact=" << exact
        << " changingInputs=" << changingInputs << " switchOwner=" << switchOwner << " lifecycle=" << lifecycle << std::endl;
    auto& device = *resources.Device;
    auto& queue = *device.GetCommandQueue();
    Set(L"Render.Particles.Scheduled", "true");
    Set(L"Render.Particles.MultiGPU", secondaryPlacement ? "true" : "false");
    Set(L"Render.Particles.Exact", exact ? "true" : "false");
    Set(L"Render.Particles.UpdateEveryNFrames", std::to_string(cadence).c_str());
    Set(L"Render.Particles.DrawEnabled", "false");
    Set(L"Render.Particles.Enabled", "true");
    Set(L"Render.Particles.SimulationEnabled", "true");
    resources.ParticleSimulation = std::make_unique<GDX12ParticleSimulation>();
    RenderPipelineCommonData common;
    common.ResetHistory = false;
    common.ActiveCameraCBufferIndex = 0;
    GDX12TextureDesc desc;
    desc.Width = desc.Height = 16;
    desc.Format = desc.SRVDesc.Format = desc.RTVDesc.Format = DXGI_FORMAT_R32_FLOAT;
    desc.SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    desc.SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    desc.SRVDesc.Texture2D.MipLevels = 1;
    desc.SRV_UAV_Heap = resources.SRV_UAV_Heap.get();
    desc.SRVHeapIndex = desc.SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength);
    desc.RTVHeap = resources.RTVHeap.get();
    desc.CreateRTV = true;
    desc.RTVHeapIndex = desc.RTVHeap->GetAvailableIndex();
    desc.RTVDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    GDX12Texture depth;
    depth.Initialize(desc);
    GDX12UploadBuffer<XMFLOAT4> interaction(&device, 4, EBufferType::Default, false), upload(&device, 4, EBufferType::Upload, false);
    interaction.CreateSRV(resources.SRV_UAV_Heap.get(),
        resources.SRV_UAV_Heap->GetAvailableIndex(TextureCube_StartIndex + TextureCube_RangeLength));
    GDX12ParticleSimulationPass simulation;
    simulation.Setup(&resources, nullptr, &common);
    simulation.SetInputs({&depth});
    GDX12ParticlesPass draw;
    draw.Setup(&resources, nullptr, &common);
    CrossAdapterParticles route(resources, secondary, runtime, simulation, draw);
    Graph graph(&device);
    graph.ImportShared(Graph::SharedResource::Scene, "Particle test scene");
    graph.ImportLink("Captured depth", &depth);
    RenderPipelineBindings bindings;
    route.Configure(graph, bindings, &depth);
    graph.Compile(false);
    GDX12ParticleEmitter emitter;
    emitter.Id = 71;
    emitter.MaxParticles = 16;
    emitter.BurstTotal = 1;
    emitter.EmissionRate = 0;
    emitter.LifetimeMin = emitter.LifetimeMax = 10;
    emitter.SpeedMin = emitter.SpeedMax = changingInputs ? 2.0f : 10.0f;
    emitter.Direction = changingInputs ? Vector3::UnitZ : Vector3::UnitX;
    emitter.ConeAngleDegrees = 0;
    emitter.Acceleration = Vector3::Zero;
    emitter.Substeps = 1;
    emitter.MaxDeltaTime = 0.1f;
    emitter.DepthCollision = changingInputs;
    emitter.CollisionRadius = 0.01f;
    emitter.InteractionEnabled = changingInputs;
    emitter.World = Matrix::CreateTranslation(0, 0, 0.4f);
    std::vector<Particle> previousDisplayed;
    UINT64 previousEmitter = 0, previousReset = 0;
    double sourceTime = 0;
    for (UINT frame = 0; frame <= 9; ++frame)
    {
        common.ResetHistory = lifecycle && frame == 3;
        if (lifecycle && frame == 4) ++emitter.ResetRevision;
        if (lifecycle && frame == 5)
        {
            route.ResetWorld();
            resources.ParticleSimulation = std::make_unique<GDX12ParticleSimulation>();
            previousDisplayed.clear();
            sourceTime = 0;
            Check(route.GetDiagnostics().SourceTime == 0 && route.GetDiagnostics().Debt == 0,
                "Particle world reset retained time or simulation debt");
        }
        if (lifecycle && frame == 7) ++emitter.Id;
        emitter.Paused = lifecycle && frame == 8;
        if (switchOwner && frame == 3) Set(L"Render.Particles.MultiGPU", "true");
        if (switchOwner && frame == 7) Set(L"Render.Particles.MultiGPU", "false");
        emitter.DeltaTime = frame == 0 ? 0 : frame == 9 ? 0.25f : 0.025f;
        sourceTime += emitter.DeltaTime;
        if (changingInputs)
        {
            emitter.World = Matrix::CreateTranslation(0.03f * frame, 0, 0.4f);
            if (frame == 3) ++emitter.BurstTotal;
        }
        resources.ParticleEmitters = {emitter};
        auto& camera = *resources.FrameConstants[resources.CurrFrameConstantsIndex]->CameraCB;
        camera.Resize(1);
        GDX12CameraConstants cameraData{};
        cameraData.NearPlane = 0.01f; cameraData.FarPlane = 100;
        if (changingInputs) cameraData.InvViewProj._43 = 0.01f * frame;
        camera.CopyData(0, cameraData);
        Check(route.PrepareFrame(common), "Scheduled particle route was inactive");
        resources.ParticleSimulation->SetInteractionField({&interaction.GetResource(), interaction.GetSRV()->GPUHandle,
            XMFLOAT4(0, 0, 2, 2), 2, -10});
        for (UINT cell = 0; cell < 4; ++cell)
            upload.CopyData(cell, changingInputs ? XMFLOAT4(frame % 2 ? 1.0f : -1.0f, 0, 0.2f, 10) : XMFLOAT4{});
        auto* commands = queue.GetCommandList();
        commands->ResourceBarrier({depth.GetResource()->GetRenderTargetBarrier(), interaction.GetResource().GetCopyDestBarrier()});
        const float clear[] = {changingInputs ? (frame % 2 ? 0.5f : 0.75f) : 0.0f, 0, 0, 0};
        commands->GetCommandList()->ClearRenderTargetView(depth.GetRTV()->CPUHandle, clear, 0, nullptr);
        commands->GetCommandList()->CopyBufferRegion(interaction.GetResource().D3DResource.Get(), 0,
            upload.GetResource().D3DResource.Get(), 0, 4 * sizeof(XMFLOAT4));
        commands = graph.ExecuteLegacySplit(commands, nullptr, [&](const auto& boundary, auto prefix)
            { return bindings.LegacyReplacements.front().Submit(boundary, prefix); });
        queue.ExecuteCommandList(commands);
        queue.Flush();
        const auto status = route.GetDiagnostics();
        Check(std::abs(status.SourceTime - sourceTime) < 1e-6, "Particle source age stopped advancing while paused");
        Check(status.Debt <= 0.25 + 3 * 0.025 + 1e-6, "Particle cadence accumulated unbounded time debt");
        const auto displayed = resources.ParticleSimulation->CaptureCheckpoint();
        const auto bytes = Read(device, *displayed.Pools.front().Buffers[0]);
        std::vector<Particle> particles(bytes.size() / sizeof(Particle));
        std::memcpy(particles.data(), bytes.data(), bytes.size());
        const auto displayedId = displayed.Emitters.front().Id;
        const auto displayedReset = displayed.Pools.front().ResetRevision;
        const bool validHistory = !common.ResetHistory && previousEmitter == displayedId &&
            previousReset == displayedReset && previousDisplayed.size() == particles.size();
        Check(displayed.ResetMotion == !validHistory, "Particle reset motion outlived displayed history reset");
        for (size_t i = 0; i < particles.size(); ++i)
        {
            const auto& current = particles[i];
            if (current.Lifetime <= 0) continue;
            const auto& previous = validHistory ? previousDisplayed[i] : current;
            const auto& expected = validHistory && previous.Seed == current.Seed && previous.Lifetime > 0 &&
                previous.Age <= current.Age ? previous : current;
            Check((Vector3(current.Previous) - Vector3(expected.Position)).Length() < 0.002f &&
                std::abs(current.PreviousAge - expected.Age) < 1e-5f &&
                std::abs(current.PreviousRotation - expected.Rotation) < 1e-5f,
                "Particle motion did not use the previous displayed snapshot");
        }
        previousDisplayed = std::move(particles);
        previousEmitter = displayedId;
        previousReset = displayedReset;
    }
    Set(L"Render.Particles.Scheduled", "false");
    Set(L"Render.Particles.MultiGPU", "false");
    Set(L"Render.Particles.UpdateEveryNFrames", "1");
    if (lifecycle) Set(L"Render.Particles.Enabled", "false");
    Check(!route.PrepareFrame(common), "Particle route failed to return ownership to primary");
    const auto checkpoint = resources.ParticleSimulation->CaptureCheckpoint();
    Check(checkpoint.Pools.size() == 1, "Particle emitter identity changed during handoff");
    const auto data = Read(device, *checkpoint.Pools.front().Buffers[0]);
    const auto counters = Read(device, *checkpoint.Pools.front().Buffers[4]);
    Result result;
    result.Particles.resize(emitter.MaxParticles);
    std::memcpy(result.Particles.data(), data.data(), data.size());
    std::memcpy(&result.Alive, counters.data() + sizeof(UINT), sizeof(UINT));
    result.Status = route.GetDiagnostics();
    Check(std::abs(result.Status.Desired - result.Status.Completed) < 1e-6,
        "Particle route handoff lost unprocessed simulation steps");
    if (result.Status.Debt != 0 || result.Status.AgeMs != 0)
        std::cerr << "Handoff: desired=" << result.Status.Desired << " completed=" << result.Status.Completed
            << " displayed=" << result.Status.Displayed << " source=" << result.Status.SourceTime
            << " displayedSource=" << result.Status.DisplayedSourceTime << " ageMs=" << result.Status.AgeMs
            << " debt=" << result.Status.Debt << '\n';
    Check(result.Status.Debt == 0 && result.Status.AgeMs == 0, "Particle handoff left stale age or debt diagnostics");
    return result;
}

void Compare(const Result& expected, const Result& actual)
{
    Check(expected.Alive == actual.Alive, "Particle route lost or duplicated burst events");
    for (size_t i = 0; i < expected.Particles.size(); ++i)
    {
        const auto& a = expected.Particles[i]; const auto& b = actual.Particles[i];
        Check(std::abs(a.Age - b.Age) < 1e-5f &&
            (Vector3(a.Position) - Vector3(b.Position)).Length() < 0.002f &&
            (Vector3(a.Velocity) - Vector3(b.Velocity)).Length() < 0.002f,
            "Particle payload differs between primary, cadence and secondary execution");
    }
}
}

int main(int argc, char** argv)
{
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    try
    {
        const bool gbv = argc == 2 && std::string_view(argv[1]) == "--gpu-validation";
        if (argc != 1 && !gbv) throw std::invalid_argument("Expected only --gpu-validation");
        ComPtr<ID3D12Debug> debug;
        ThrowIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
        debug->EnableDebugLayer();
        if (gbv)
        {
            ComPtr<ID3D12Debug1> validation;
            ThrowIfFailed(debug.As(&validation));
            validation->SetEnableGPUBasedValidation(TRUE);
        }
        ConsoleModule{}.Initialize();
        TestApp app;
        GDX12Device primary, secondary;
        ThrowIfFailed(primary.Initialize(GDX12DeviceFactory::GetMostPerformantAdapter()));
        secondary.Role = DEVICE_ROLE_SECONDARY;
        ThrowIfFailed(secondary.Initialize(OtherAdapter(primary.GetAdapterLuid())));
        std::cout << "Primary: " << primary.GetDeviceFeatures().Name << "\nSecondary: " << secondary.GetDeviceFeatures().Name << '\n';
        GDX12CommandQueue copyA(&primary, D3D12_COMMAND_LIST_TYPE_COPY, true), copyB(&secondary, D3D12_COMMAND_LIST_TYPE_COPY, true);
        GDX12DeviceResources resources;
        resources.Initialize(&primary);
        RenderMGPURuntime runtime({&primary, &secondary, &copyA, &copyB});
        for (const bool changing : {false, true})
        {
            const auto reference = Run(resources, secondary, runtime, false, 1, true, changing, false);
            Check(reference.Alive == (changing ? 2u : 1u), "Independent burst count failed");
            if (!changing)
            {
                const auto alive = std::find_if(reference.Particles.begin(), reference.Particles.end(),
                    [](const auto& p) { return p.Lifetime > 0; });
                Check(alive != reference.Particles.end() && std::abs(alive->Age - 0.45f) < 1e-5f &&
                    std::abs(alive->Position.x - 4.5f) < 0.001f, "Scheduled simulation discarded elapsed time");
            }
            Compare(reference, Run(resources, secondary, runtime, false, 4, true, changing, false));
            for (const bool exact : {true, false})
                for (const UINT cadence : {1u, 4u})
                    Compare(reference, Run(resources, secondary, runtime, true, cadence, exact, changing, false));
            Compare(reference, Run(resources, secondary, runtime, false, 2, true, changing, true));
            const auto reset = Run(resources, secondary, runtime, false, 1, true, changing, false, true);
            Compare(reset, Run(resources, secondary, runtime, false, 4, true, changing, false, true));
            Compare(reset, Run(resources, secondary, runtime, true, 4, false, changing, false, true));
        }
        runtime.Shutdown();
        GDX12ShaderCompiler::Shutdown();
        CheckDebug(primary); CheckDebug(secondary);
        std::cout << "Particle MGPU checks passed: time, bursts, input snapshots, cadence, Latest and ownership.\n";
        return 0;
    }
    catch (const DxException& error) { std::wcerr << error.ToString() << '\n'; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
    return 1;
}
