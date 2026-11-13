#include "Techniques/SceneEnvironment.h"
#include "Graph/MetaGraphExecutor.h"
#include "Engine.Core/Logging/Logger.h"
#include "Common/RuntimePaths.h"
#include <fstream>
#include <algorithm>
#include <cstring>

namespace
{
struct Readback
{
    GDX12ResourceHandle Buffer;
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> Footprints;
    std::vector<UINT> Rows;
    std::vector<UINT64> RowBytes;
    UINT64 Bytes = 0;
};
std::array<Readback, 4> AllocateReadback(GDX12Device &device, GDX12RenderPass *reference,
                                         Meta::SceneEnvironment *native)
{
    std::array<Readback, 4> result;
    for (UINT i = 0; i < 4; ++i)
    {
        auto *texture = (reference ? reference->GetOutput(i) : native->Output(i))->GetTexture();
        const auto shape = texture->GetResource()->D3DResource->GetDesc();
        const UINT count = shape.MipLevels * shape.DepthOrArraySize;
        auto &b = result[i];
        b.Footprints.resize(count);
        b.Rows.resize(count);
        b.RowBytes.resize(count);
        device.GetDevice()->GetCopyableFootprints(&shape, 0, count, 0, b.Footprints.data(), b.Rows.data(),
                                                  b.RowBytes.data(), &b.Bytes);
        b.Buffer = device.CreateResource(D3D12_HEAP_TYPE_READBACK, CD3DX12_RESOURCE_DESC::Buffer(b.Bytes),
                                         D3D12_RESOURCE_STATE_COPY_DEST);
    }
    return result;
}
void Copy(ID3D12GraphicsCommandList *cmd, ID3D12Resource *source, const Readback &b)
{
    for (UINT i = 0; i < b.Footprints.size(); ++i)
    {
        const CD3DX12_TEXTURE_COPY_LOCATION src(source, i), dst(b.Buffer.Get(), b.Footprints[i]);
        cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
}
std::vector<uint8_t> Collect(const std::array<Readback, 4> &data, const std::string &stem)
{
    std::vector<uint8_t> result;
    for (UINT i = 0; i < 4; ++i)
    {
        const auto &b = data[i];
        void *mapped = nullptr;
        const D3D12_RANGE range{0, SIZE_T(b.Bytes)}, noWrite{};
        ThrowIfFailed(b.Buffer->Map(0, &range, &mapped));
        for (UINT sub = 0; sub < b.Footprints.size(); ++sub)
            for (UINT y = 0; y < b.Rows[sub]; ++y)
            {
                const auto *row = static_cast<uint8_t *>(mapped) + b.Footprints[sub].Offset +
                                  UINT64(y) * b.Footprints[sub].Footprint.RowPitch;
                result.insert(result.end(), row, row + b.RowBytes[sub]);
            }
        b.Buffer->Unmap(0, &noWrite);
    }
    std::ofstream file(stem + ".bin", std::ios::binary);
    file.write(reinterpret_cast<const char *>(result.data()), result.size());
    return result;
}

std::vector<uint8_t> NativeCapture(GDX12DeviceResources &r, Meta::SceneEnvironment &native, const std::string &stem,
                                   bool serial)
{
    auto readback = AllocateReadback(*r.Device, nullptr, &native);
    Meta::Graph graph;
    std::vector<ID3D12Resource *> bindings;
    native.Append(graph, bindings);
    const Meta::Id firstRead = Meta::Id(graph.Operations.size());
    for (UINT i = 0; i < 4; ++i)
    {
        auto *source = native.Output(i)->GetTexture()->GetResource()->D3DResource.Get();
        const auto found = std::find(bindings.begin(), bindings.end(), source);
        const auto id = Meta::Id(found - bindings.begin());
        if (found == bindings.end())
        {
            bindings.push_back(source);
            graph.Resources.push_back({"cached environment", 0, 0, true});
        }
        const auto read = Meta::Id(bindings.size());
        bindings.push_back(readback[i].Buffer.Get());
        graph.Resources.push_back({"readback", D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_DEST});
        graph.Operations.push_back({"environment lifecycle readback",
                                    {{id, Meta::Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE},
                                     {read, Meta::Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST}}});
    }
    const auto schedule = Meta::Compile(graph);
    Meta::Executor executor(*r.Device, 3, 192);
    executor.Begin(r.CurrFrameConstantsIndex);
    std::ofstream plan(stem + "-schedule.txt");
    schedule.Write(plan);
    native.Submitted(executor.Execute(
        schedule, bindings,
        [&](Meta::Id op, UINT, auto *cmd) {
            if (!native.Record(op, cmd))
                Copy(cmd, native.Output(op - firstRead)->GetTexture()->GetResource()->D3DResource.Get(),
                     readback[op - firstRead]);
        },
        nullptr, nullptr, serial));
    executor.Drain();
    return Collect(readback, stem);
}
std::vector<uint8_t> OriginalCapture(GDX12DeviceResources &r, GDX12EnvironmentIBLPass &baseline,
                                     const std::string &stem)
{
    auto *commands = r.Device->GetCommandQueue()->GetCommandList();
    baseline.Execute(commands);
    auto readback = AllocateReadback(*r.Device, &baseline, nullptr);
    for (UINT i = 0; i < 4; ++i)
    {
        auto *texture = baseline.GetOutput(i)->GetTexture();
        commands->ResourceBarrier({texture->GetResource()->GetCopySourceBarrier()});
        Copy(commands->GetCommandList().Get(), texture->GetResource()->D3DResource.Get(), readback[i]);
    }
    r.Device->GetCommandQueue()->ExecuteCommandList(commands);
    r.Device->GetCommandQueue()->Flush();
    return Collect(readback, stem);
}
void CloudLifecycle(GDX12DeviceResources &r, RenderPipelineCommonData &common, const std::filesystem::path &root)
{
    r.Sky = {};
    r.Sky.Quality = 1;
    r.Sky.Box.IBL = {16, 16, 1};
    Meta::SceneEnvironment native(r, common);
    GDX12EnvironmentIBLPass baseline;
    GDX12CloudComponent clouds;
    baseline.Setup(&r, &common);
    baseline.Initialize();
    clouds.Setup(&r, &common);
    baseline.SetCloudSource(&clouds);
    GDX12CloudParameters parameters;
    parameters.Octaves = 3;
    native.SetClouds(parameters);
    clouds.SetParameters(parameters);
    std::vector<uint8_t> first, previous;
    std::ofstream metrics(root / "environment-lifecycle.txt");
    for (UINT step = 0; step < 8; ++step)
    {
        if (step == 3)
        {
            native.ResetClouds();
            clouds.Reset();
        }
        if (step == 4 || step == 5)
        {
            parameters.Enabled = step == 5;
            native.SetClouds(parameters);
            clouds.SetParameters(parameters);
        }
        // Caller resolves simulation time as world dt * time scale, or zero
        // while paused, exactly as Bistro. Last two steps compare 1x and 2x.
        const double delta = step == 2 || step == 6 ? .125 : step == 7 ? .250 : 0;
        native.Freeze(delta);
        clouds.Advance(delta);
        const auto stem = (root / ("environment-lifecycle-" + std::to_string(step))).string();
        const auto serial = NativeCapture(r, native, stem + "-serial", true);
        const auto parallel = NativeCapture(r, native, stem + "-parallel", false);
        const auto reference = OriginalCapture(r, baseline, stem + "-baseline");
        if (serial != parallel || serial != reference)
            throw std::runtime_error("Cloud lifecycle baseline mismatch step=" + std::to_string(step));
        if (step == 0)
            first = serial;
        if ((step == 1 || step == 3 || step == 5) && serial != first)
            throw std::runtime_error("Cloud pause/cache/reset changed source phase");
        if ((step == 2 || step == 4 || step == 6 || step == 7) && serial == previous)
            throw std::runtime_error("Cloud time/enable change left stale output");
        metrics << "step=" << step << " simulation_seconds=" << native.CloudInputs().SimulationSeconds
                << " baseline_max_error=0 serial_parallel_max_error=0\n";
        previous = serial;
    }
    baseline.ClearDenendencies();
    clouds.ClearDenendencies();
}
} // namespace
void RunEnvironmentGpuTests(GDX12DeviceResources &r, RenderPipelineCommonData &common)
{
    const auto savedSky = r.Sky;
    const auto root = Engine::Core::GetExecutableDirectory() / "Logs";
    std::ofstream metrics(root / "environment-metrics.txt");
    // Every native output is compared with the original environment algorithm,
    // not just with another mode of the new executor. Both use the same camera,
    // author settings, simulation phase and original shader binaries.
    for (UINT scenario = 0; scenario < 5; ++scenario)
    {
        r.Sky = {};
        r.Sky.Revision = scenario + 1;
        r.Sky.Quality = 1;
        r.Sky.Source = scenario == 0 ? SkySource::Off : scenario == 1 ? SkySource::SkyBox : SkySource::SkyAtmosphere;
        r.Sky.Box.IBL = {16, 16, 1};
        auto &a = r.Sky.Atmosphere;
        a.IBL = {16, 16, 1};
        a.TransmittanceWidth = 32;
        a.TransmittanceHeight = 8;
        a.MultipleScatteringSize = 8;
        a.SkyViewWidth = 32;
        a.SkyViewHeight = 16;
        a.AerialWidth = 8;
        a.AerialHeight = 8;
        a.AerialDepth = 8;
        a.TransmittanceSamples = 8;
        a.SkySamples = 8;
        a.MultipleScatteringDirections = 8;
        a.MultipleScatteringSamples = 8;
        a.AerialSamples = 4;
        GDX12CloudParameters parameters;
        parameters.Enabled = scenario >= 3;
        parameters.Octaves = 3;
        const double delta = scenario == 4 ? 0.125 : 0;
        Meta::SceneEnvironment native(r, common);
        if (parameters.Enabled)
            native.SetClouds(parameters);
        native.Freeze(delta);
        auto readback = AllocateReadback(*r.Device, nullptr, &native);
        Meta::Graph graph;
        std::vector<ID3D12Resource *> bindings;
        native.Append(graph, bindings);
        const Meta::Id firstRead = Meta::Id(graph.Operations.size());
        for (UINT i = 0; i < 4; ++i)
        {
            auto *source = native.Output(i)->GetTexture()->GetResource()->D3DResource.Get();
            auto found = std::find(bindings.begin(), bindings.end(), source);
            Meta::Id id = Meta::Id(found - bindings.begin());
            if (found == bindings.end())
            {
                bindings.push_back(source);
                graph.Resources.push_back({"cached environment output", 0, 0, true});
            }
            const auto read = Meta::Id(bindings.size());
            bindings.push_back(readback[i].Buffer.Get());
            graph.Resources.push_back(
                {"environment readback", D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_DEST});
            graph.Operations.push_back({"environment output readback",
                                        {{id, Meta::Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE},
                                         {read, Meta::Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST}}});
        }
        const auto schedule = Meta::Compile(graph);
        Meta::Executor executor(*r.Device, 3, 192);
        std::ofstream plan(root / ("environment-" + std::to_string(scenario) + "-schedule.txt"));
        schedule.Write(plan);
        std::ofstream operations(root / ("environment-" + std::to_string(scenario) + "-operations.txt"));
        for (Meta::Id i = 0; i < graph.Operations.size(); ++i)
            operations << i << ' ' << graph.Operations[i].Name << '\n';
        std::vector<uint8_t> serial;
        for (bool parallel : {false, true})
        {
            executor.Begin(parallel ? 1 : 0);
            std::ofstream trace(root / ("environment-" + std::to_string(scenario) +
                                        (parallel ? "-parallel-cpu.json" : "-serial-cpu.json")));
            ComPtr<ID3D12Fence> gate;
            struct ReleaseGate
            {
                ComPtr<ID3D12Fence> &Fence;
                ~ReleaseGate()
                {
                    if (Fence)
                        Fence->Signal(1);
                }
            } releaseGate{gate};
            if (scenario == 0 && !parallel)
            {
                ThrowIfFailed(r.Device->GetDevice()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)));
                ThrowIfFailed(r.Device->GetCommandQueue()->GetCommandQueue()->Wait(gate.Get(), 1));
            }
            const auto fence = executor.Execute(
                schedule, bindings,
                [&](Meta::Id op, UINT, auto *cmd) {
                    if (!native.Record(op, cmd))
                        Copy(cmd, native.Output(op - firstRead)->GetTexture()->GetResource()->D3DResource.Get(),
                             readback[op - firstRead]);
                },
                nullptr, nullptr, !parallel, nullptr, &trace);
            native.Submitted(fence);
            if (gate)
            {
                const bool premature = native.Ready();
                ThrowIfFailed(gate->Signal(1));
                if (premature)
                    throw std::runtime_error("CPU-ready environment published before GPU completion");
            }
            executor.Drain();
            if (!native.Ready())
                throw std::runtime_error("Completed environment not ready");
            auto bytes = Collect(
                readback,
                (root / ("environment-" + std::to_string(scenario) + (parallel ? "-parallel" : "-serial"))).string());
            if (!parallel)
                serial = std::move(bytes);
            else if (bytes != serial)
                throw std::runtime_error("Environment serial/parallel mismatch");
        }
        GDX12EnvironmentIBLPass baseline;
        GDX12CloudComponent clouds;
        baseline.Setup(&r, &common);
        baseline.Initialize();
        if (parameters.Enabled)
        {
            clouds.Setup(&r, &common);
            clouds.SetParameters(parameters);
            clouds.Advance(delta);
            baseline.SetCloudSource(&clouds);
        }
        auto *commands = r.Device->GetCommandQueue()->GetCommandList();
        baseline.Execute(commands);
        auto baselineReadback = AllocateReadback(*r.Device, &baseline, nullptr);
        for (UINT i = 0; i < 4; ++i)
        {
            auto *texture = baseline.GetOutput(i)->GetTexture();
            commands->ResourceBarrier({texture->GetResource()->GetCopySourceBarrier()});
            Copy(commands->GetCommandList().Get(), texture->GetResource()->D3DResource.Get(), baselineReadback[i]);
        }
        r.Device->GetCommandQueue()->ExecuteCommandList(commands);
        r.Device->GetCommandQueue()->Flush();
        const auto reference =
            Collect(baselineReadback, (root / ("environment-" + std::to_string(scenario) + "-baseline")).string());
        if (serial != reference)
            throw std::runtime_error("Environment original-pass baseline mismatch scenario=" +
                                     std::to_string(scenario));
        metrics << "scenario=" << scenario << " bytes=" << serial.size()
                << " serial_parallel_max_error=0 baseline_max_error=0\n";
        baseline.ClearDenendencies();
        clouds.ClearDenendencies();
    }
    CloudLifecycle(r, common, root);
    r.Sky = savedSky;
    Log::Info("MetaRenderGraph", "PASS EnvironmentGpuTests: Off/DDS/atmosphere/clouds/advanced phase; original "
                                 "baseline and serial/parallel exact");
}
