#include "App.Base/App.h"
#include "Rendering/Water/CrossAdapterWater.h"
#include <d3d12sdklayers.h>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace
{
using namespace GDX12Submission;
using HostClock = std::chrono::steady_clock;
using Particle = GDX12FluidSolver::Particle;

void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

class FixtureApp final : public App
{
public:
    FixtureApp() : App(GetModuleHandleW(nullptr)) {}
};

class Gate final
{
public:
    Gate(GDX12Device& device, GDX12CommandQueue& queue)
    {
        ThrowIfFailed(device.GetDevice()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_fence)));
        ThrowIfFailed(queue.GetCommandQueue()->Wait(_fence.Get(), 1));
    }
    ~Gate() { if (_fence) _fence->Signal(1); }
    void Open() { ThrowIfFailed(_fence->Signal(1)); _fence.Reset(); }
private:
    ComPtr<ID3D12Fence> _fence;
};

void Wait(GDX12CommandQueue& queue, CompletionPoint point)
{
    Require(point.IsValid() && point.Timeline() == queue.GetTimelineIdentity(), "Water fixture has the wrong timeline");
    const auto deadline = HostClock::now() + std::chrono::seconds(10);
    for (;;)
    {
        const auto progress = queue.GetCompletionProgress();
        if (point.IsReachedBy(progress)) return;
        if (progress.CompletedValue == UINT64_MAX) ThrowIfFailed(DXGI_ERROR_DEVICE_REMOVED);
        Require(HostClock::now() < deadline, "Water fixture timed out waiting for its receipt");
        Sleep(1);
    }
}

Particle ReadFrame(CrossAdapterWater& water, GDX12Device& device, Particle* previous = nullptr)
{
    const auto& display = water.Display();
    Require(display && display->Count == 1, "Water fixture has no one-particle display");
    auto& queue = *device.GetCommandQueue();
    const auto bytes = (previous ? 2 : 1) * sizeof(Particle);
    const auto readback = device.CreateResource(D3D12_HEAP_TYPE_READBACK,
        CD3DX12_RESOURCE_DESC::Buffer(bytes), D3D12_RESOURCE_STATE_COPY_DEST);
    auto* commands = queue.GetCommandList();
    const auto copy = [&](GDX12Resource& source, UINT64 offset)
    {
        commands->ResourceBarrier({source.GetCopySourceBarrier()});
        commands->GetCommandList()->CopyBufferRegion(readback.Get(), offset, source.D3DResource.Get(), 0, sizeof(Particle));
        commands->ResourceBarrier({source.GetCommonBarrier()});
    };
    copy(*display->Current.Resource, 0);
    if (previous)
    {
        Require(display->HistoryValid, "Expected previous Water display");
        copy(*display->Previous.Resource, sizeof(Particle));
    }
    queue.ExecuteCommandList(commands);
    const auto tail = queue.SignalCompletion();
    water.RetireFrame(tail);
    Wait(queue, tail);
    const D3D12_RANGE read{0, bytes}, noWrite{0, 0};
    void* mapped = nullptr;
    ThrowIfFailed(readback->Map(0, &read, &mapped));
    Particle particle{};
    std::memcpy(&particle, mapped, sizeof(Particle));
    if (previous) std::memcpy(previous, static_cast<std::byte*>(mapped) + sizeof(Particle), sizeof(Particle));
    readback->Unmap(0, &noWrite);
    return particle;
}

void CheckWater(CrossAdapterWater& water, GDX12DeviceResources& primary,
    const RenderMGPUDevices& devices, RenderMGPURuntime& runtime)
{
    auto& direct = *devices.Primary->GetCommandQueue();
    auto& compute = *devices.Secondary->TryGetComputeQueue();
    auto& upload = *devices.SecondaryCopy;
    auto& download = *devices.PrimaryCopy;
    GDX12FluidInstance instance;
    instance.Id = 1;
    instance.DeltaSeconds = 1.0f / 60;
    GDX12FluidSettings settings;
    settings.MGPUReference = true;
    settings.AccelerationCmPerSecond2 = Vector3::Zero;
    instance.Component.SetSettings(settings);
    instance.Component.SetParticles({{Vector3::Zero, Vector3(1, 0, 0)}});
    GDX12FluidControls controls{true, 2};
    {
        CrossAdapterWater unavailable;
        auto requested = controls;
        requested.MultiGPU = true;
        unavailable.Tick(&instance, requested, nullptr, primary, nullptr, true);
        unavailable.Tick(&instance, requested, &runtime, primary, devices.Secondary, true);
        Require(!unavailable.RoutedOwner() && unavailable.GetDiagnostics().ResetRequired,
            "A late secondary replaced the startup local authority");
    }
    std::uint64_t frame = 0;
    const auto tick = [&](bool surfaceEligible = true)
    {
        runtime.Poll();
        water.Tick(&instance, controls, &runtime, primary, devices.Secondary, surfaceEligible);
        water.SelectFrame(FrameSubmissionId{++frame});
    };
    const auto retire = [&]
    {
        const auto tail = direct.SignalCompletion();
        water.RetireFrame(tail);
        Wait(direct, tail);
    };

    tick();
    Require(!water.RoutedOwner() && !water.Display(), "Water routing must default to local");
    controls.MultiGPU = true;
    tick();
    Require(!water.RoutedOwner() && water.GetDiagnostics().ResetRequired, "Route switch replaced a live local epoch");

    instance.Component.Reset();
    const auto beforeImport = download.GetLastCompletionPoint().Value();
    {
        Gate holdExport(*devices.Secondary, upload);
        tick();
        Require(water.RoutedOwner() == instance.Id && !water.Display(), "Latest chose an unfinished import");
        Wait(compute, compute.GetLastCompletionPoint());
        tick();
        const auto progress = water.GetDiagnostics();
        Require(!water.Display() && progress.SkippedPublications == 1 &&
            progress.Issued > progress.Completed, "Held publication did not permit a solver-only invocation");
        Require(download.GetLastCompletionPoint().Value() == beforeImport, "Latest queued an unfinished export on primary COPY");
        retire();
        holdExport.Open();
    }

    instance.Paused = true;
    const auto firstDeadline = HostClock::now() + std::chrono::seconds(10);
    for (;;)
    {
        tick();
        if (water.Display()) break;
        Require(HostClock::now() < firstDeadline, "Latest Water did not display its completed import");
        Sleep(1);
    }
    Require(!water.Display()->HistoryValid, "First Water display inherited unrelated history");
    const auto latest = ReadFrame(water, *devices.Primary);
    Require(latest.Position.x > 0, "Latest Water imported a seed instead of the first logical step");

    const auto previousTransform = instance.ContainerToWorld;
    instance.ContainerToWorld = Matrix::CreateTranslation(10, 0, 0);
    instance.Paused = false;
    controls.MultiGPUUpdateHz = 0.001f;
    controls.MultiGPUMaxAgeMs = 0.001f;
    const auto lastInvocations = water.GetDiagnostics().SolverInvocations;
    Sleep(2);
    tick();
    Require(water.Display() && water.Display()->HistoryValid &&
        water.Display()->PreviousTransform == previousTransform &&
        water.Display()->Transform == instance.ContainerToWorld, "Repeated snapshot lost actual displayed transforms");
    Require(water.GetDiagnostics().SolverInvocations == lastInvocations && water.GetDiagnostics().OverAge,
        "Cadence or selected snapshot age did not use monotonic time");
    const auto repeated = ReadFrame(water, *devices.Primary);
    Require(std::memcmp(&latest, &repeated, sizeof(Particle)) == 0, "Repeat changed the displayed particle payload");

    controls.MultiGPUUpdateHz = 0;
    const auto beforeLatestHidden = water.GetDiagnostics().Issued;
    const auto beforeLatestExport = upload.GetLastCompletionPoint().Value();
    tick(false);
    const auto latestHidden = water.GetDiagnostics();
    Require(latestHidden.Issued > beforeLatestHidden && upload.GetLastCompletionPoint().Value() == beforeLatestExport,
        "Latest did not continue solver-only when Surface was absent");
    Require(!water.Display() && latestHidden.SnapshotAgeSeconds == -1 && !latestHidden.OverAge,
        "Latest counted a ready snapshot as displayed without Surface");
    retire();
    Wait(compute, compute.GetLastCompletionPoint());
    instance.Paused = true;
    tick();
    Require(water.Display() && !water.Display()->HistoryValid, "Latest carried motion history across an absent Surface");
    const auto resumedLatest = ReadFrame(water, *devices.Primary);
    Require(std::memcmp(&latest, &resumedLatest, sizeof(Particle)) == 0,
        "Latest replaced its ready snapshot during a solver-only frame");
    instance.Paused = false;
    controls.MultiGPUUpdateHz = 0.001f;

    controls.MultiGPUExact = true;
    tick();
    Require(!water.GetDiagnostics().Exact && water.GetDiagnostics().ResetRequired,
        "Freshness switch replaced a live secondary epoch");
    retire();
    controls.MultiGPUUpdateHz = 0;
    {
        Gate holdImport(*devices.Primary, download);
        const auto before = download.GetLastCompletionPoint().Value();
        const auto deadline = HostClock::now() + std::chrono::seconds(10);
        do
        {
            tick();
            retire();
            Require(HostClock::now() < deadline, "Latest import was never issued");
            Sleep(1);
        } while (download.GetLastCompletionPoint().Value() == before);
        instance.Component.Reset();
        instance.Paused = true;
        tick();
        Require(water.GetDiagnostics().Exact && !water.Display() && !water.WantsExactBoundary(),
            "Reset during import exposed the prior epoch or scheduled a paused seed");
        holdImport.Open();
    }

    const auto seekExact = [&]
    {
        const auto deadline = HostClock::now() + std::chrono::seconds(10);
        for (;;)
        {
            tick();
            if (water.WantsExactBoundary()) break;
            // Bank retirement can take several host frames. Keep the original logical interval.
            instance.DeltaSeconds = 0;
            Require(HostClock::now() < deadline, "Exact Water did not become eligible after reset");
            Sleep(1);
        }
    };
    instance.Paused = false;
    seekExact();
    {
        Gate holdExport(*devices.Secondary, upload);
        const auto prefix = direct.SignalCompletion();
        const auto handoff = water.SubmitExact(prefix);
        Require(handoff && water.Display() && !water.Display()->HistoryValid,
            "Exact Water did not admit its first compatible display");
        Wait(direct, prefix);
        Require(!handoff->IsReachedBy(direct.GetCompletionProgress()), "Exact handoff overtook secondary export");
        holdExport.Open();
        const auto exact = ReadFrame(water, *devices.Primary);
        Require(std::memcmp(&latest, &exact, sizeof(Particle)) == 0,
            "Exact and Latest disagree at the same logical Water step");
    }
    instance.DeltaSeconds = 1.0f / 60;
    tick();
    Require(water.WantsExactBoundary() && water.SubmitExact(direct.SignalCompletion()),
        "Second Exact Water invocation did not submit");
    Particle previous{};
    const auto next = ReadFrame(water, *devices.Primary, &previous);
    Require(std::memcmp(&latest, &previous, sizeof(Particle)) == 0 && next.Position.x > previous.Position.x,
        "Second Exact Water invocation lost the previous displayed payload");
    instance.Component.Reset();
    instance.DeltaSeconds = 1.0f / 30;
    seekExact();
    Require(water.SubmitExact(direct.SignalCompletion()).has_value(), "30 Hz Exact Water invocation did not submit");
    const auto paired = ReadFrame(water, *devices.Primary);
    Require(std::memcmp(&next, &paired, sizeof(Particle)) == 0, "60 Hz and 30 Hz disagree at two logical Water steps");
    instance.DeltaSeconds = 0;
    settings.RenderEnabled = false;
    instance.Component.SetSettings(settings);
    tick();
    Require(!water.Display() && !water.WantsExactBoundary(), "Hidden Water acquired an output");
    retire();
    settings.RenderEnabled = true;
    instance.Component.SetSettings(settings);
    const auto beforeReveal = water.GetDiagnostics().SolverInvocations;
    tick();
    Require(water.WantsExactBoundary() && water.SubmitExact(direct.SignalCompletion()),
        "Visible Water could not publish without a new solver step");
    Require(!water.Display()->HistoryValid && water.GetDiagnostics().SolverInvocations == beforeReveal,
        "Water reveal advanced simulation or reused hidden-frame motion history");
    const auto revealed = ReadFrame(water, *devices.Primary);
    Require(std::memcmp(&paired, &revealed, sizeof(Particle)) == 0, "Water reveal changed the solver payload");

    const auto beforeExactHidden = water.GetDiagnostics();
    const auto beforeExactExport = upload.GetLastCompletionPoint().Value();
    instance.DeltaSeconds = 1.0f / 60;
    for (int i = 0; i < 3; ++i)
    {
        const auto before = water.GetDiagnostics().Issued;
        tick(false);
        Require(!water.WantsExactBoundary() && !water.Display() && water.GetDiagnostics().Issued > before,
            "Exact did not advance without its Surface cut");
        retire();
        Wait(compute, compute.GetLastCompletionPoint());
    }
    Require(upload.GetLastCompletionPoint().Value() == beforeExactExport, "Exact published with no Surface");
    instance.DeltaSeconds = 0;
    tick();
    const auto afterExactHidden = water.GetDiagnostics();
    Require(afterExactHidden.Completed > beforeExactHidden.Completed &&
        afterExactHidden.SolverInvocations == beforeExactHidden.SolverInvocations + 3,
        "Exact solver-only receipts did not advance the completed cursor");
    Require(water.WantsExactBoundary() && water.SubmitExact(direct.SignalCompletion()),
        "Exact did not publish its advanced state when Surface returned");
    Require(!water.Display()->HistoryValid, "Exact carried motion history across an absent Surface");
    const auto resumedExact = ReadFrame(water, *devices.Primary);
    Require(resumedExact.Position.x > revealed.Position.x, "Resumed Exact display omitted hidden simulation");
    std::cout << "Water Latest, held source, history, reset, Exact receipts, 60/30 Hz parity, and absent Surface passed.\n";
}

void CheckDebug(GDX12Device& device)
{
    ComPtr<ID3D12InfoQueue> queue;
    ThrowIfFailed(device.GetDevice().As(&queue));
    for (UINT64 i = 0; i < queue->GetNumStoredMessages(); ++i)
    {
        SIZE_T bytes = 0;
        ThrowIfFailed(queue->GetMessage(i, nullptr, &bytes));
        std::vector<std::byte> storage(bytes);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        ThrowIfFailed(queue->GetMessage(i, message, &bytes));
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) throw std::runtime_error(message->pDescription);
    }
}
}

int main(int argc, char** argv)
{
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    try
    {
        Require(argc == 1 || (argc == 2 && std::string_view(argv[1]) == "--gpu-validation"), "Unknown Water fixture option");
        ComPtr<ID3D12Debug> debug;
        ThrowIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
        debug->EnableDebugLayer();
        if (argc == 2)
        {
            ComPtr<ID3D12Debug1> gbv;
            ThrowIfFailed(debug.As(&gbv));
            gbv->SetEnableGPUBasedValidation(TRUE);
        }
        ConsoleModule{}.Initialize();
        FixtureApp app;
        Require(app.Initialize(), "Water fixture application startup failed");
        const auto render = BenchmarkEngine::GetLocator().GetModule<RenderModule>();
        const auto devices = render->GetMGPUDevices();
        const auto& status = render->GetMGPUStatus();
        auto* runtime = render->GetMGPURuntime();
        Require(runtime && devices.Primary && devices.Secondary && devices.PrimaryCopy && devices.SecondaryCopy &&
            devices.Secondary->TryGetComputeQueue() && status.PrimaryLuid != status.ActiveSecondaryLuid,
            "Water fixture needs two physical adapters");
        render->WaitForUploads();
        GDX12DeviceResources primary;
        primary.Device = devices.Primary;
        primary.SRV_UAV_Heap = std::make_unique<GDX12DescriptorHeap>(primary.Device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
            TextureResources_StartIndex + TextureResources_RangeLength, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
        CrossAdapterWater water;
        try
        {
            CheckWater(water, primary, devices, *runtime);
            CheckDebug(*devices.Primary);
            CheckDebug(*devices.Secondary);
        }
        catch (...)
        {
            render->ShutdownMGPU();
            throw;
        }
        Require(render->ShutdownMGPU(), "Water fixture failed to retire its MGPU runtime");
        return 0;
    }
    catch (const DxException& error) { std::wcerr << error.ToString() << '\n'; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
    return 1;
}
