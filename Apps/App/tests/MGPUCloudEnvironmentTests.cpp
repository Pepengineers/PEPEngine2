#include "App.Base/App.h"
#include "App.Base/Rendering/CrossAdapterEnvironment.h"
#include "App.Base/Rendering/RenderMGPURuntime.h"
#include "Engine.Core/ConsoleModule.h"
#include "Engine.RendererDX12/GDX12SnapshotTransport.h"
#include "Engine.RendererDX12/RenderPasses/GDX12EnvironmentIBLPass.h"
#include <DirectXPackedVector.h>
#include <d3d12sdklayers.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>

using namespace GDX12Snapshot;

namespace
{
    class TestApp final : public App
    {
    public:
        TestApp() : App(GetModuleHandleW(nullptr)) {}
    };

    void Check(bool condition, const char* expression, int line)
    {
        if (!condition) throw std::runtime_error("line " + std::to_string(line) + ": " + expression);
    }
#define CHECK(...) Check(static_cast<bool>(__VA_ARGS__), #__VA_ARGS__, __LINE__)

    void CheckInputsAndCadence()
    {
        ConsoleModule console;
        CHECK(!console.FindConsoleVariable(L"Render.Clouds.Bistro").GetBool());
        CHECK(!console.FindConsoleVariable(L"Render.Clouds.AsyncCompute").GetBool());
        CHECK(!console.FindConsoleVariable(L"Render.Clouds.MultiGPU").GetBool());
        CHECK(console.FindConsoleVariable(L"Render.Clouds.Latest").GetBool());
        CHECK(GetCloudPublicationSettings().UpdateHz == 0 && GetCloudPublicationSettings().MaxAgeMs == 0);
        auto& rate = console.FindConsoleVariable(L"Render.Clouds.UpdateHz");
        auto& age = console.FindConsoleVariable(L"Render.Clouds.MaxAgeMs");
        CHECK(!rate.SetFromString("nan") && !rate.SetFromString("1001"));
        CHECK(!age.SetFromString("-1") && !age.SetFromString("60001"));
        CHECK(rate.SetFromString("55") && age.SetFromString("12"));
        CHECK(GetCloudPublicationSettings().UpdateHz == 55 && GetCloudPublicationSettings().MaxAgeMs == 12);
        CHECK(rate.SetFromString("0") && age.SetFromString("0"));
        GDX12DeviceResources resources;
        RenderPipelineCommonData common;
        GDX12EnvironmentIBLPass environment;
        GDX12CloudComponent clouds;
        environment.Setup(&resources, nullptr, &common);
        environment.SetCloudSource(&clouds);
        const auto initial = environment.CaptureCloudInputs().value();
        clouds.Advance(0);
        CHECK(environment.CaptureCloudInputs()->Cloud.LightingRevision == initial.Cloud.LightingRevision);
        clouds.Advance(0.5);
        const auto advanced = environment.CaptureCloudInputs().value();
        CHECK(advanced.IsCompatibleWith(initial));
        CHECK(advanced.Cloud.LightingRevision > initial.Cloud.LightingRevision);
        CHECK(advanced.Cloud.SimulationSeconds == 0.5 && initial.Cloud.SimulationSeconds == 0.0);
        CHECK(advanced.Cloud.Phase.x != initial.Cloud.Phase.x);
        CHECK(advanced.CapturedAt >= initial.CapturedAt);
        const auto shortlyAfter = initial.CapturedAt + std::chrono::milliseconds(12);
        CHECK(initial.MeasureAge(shortlyAfter, 12).Milliseconds == 12);
        CHECK(!initial.MeasureAge(shortlyAfter, 12).OverAge);
        CHECK(initial.MeasureAge(shortlyAfter, 10).OverAge);
        CHECK(!initial.MeasureAge(shortlyAfter, 0).OverAge);

        resources.Lighting.LightDirection.x += 0.25f;
        CHECK(!environment.CaptureCloudInputs()->IsCompatibleWith(advanced));
        const auto lit = environment.CaptureCloudInputs().value();
        clouds.Reset();
        CHECK(!environment.CaptureCloudInputs()->IsCompatibleWith(lit));
        CHECK(environment.CaptureCloudInputs()->Cloud.SimulationSeconds == 0);
        auto settings = clouds.GetParameters();
        settings.Seed = 942;
        clouds.SetParameters(settings);
        const auto seeded = environment.CaptureCloudInputs().value();
        CHECK(initial.Cloud.Parameters.Seed == 0 && seeded.Cloud.Parameters.Seed == 942);
        environment.Resize();
        CHECK(!environment.CaptureCloudInputs()->IsCompatibleWith(seeded));
        settings.Enabled = false;
        clouds.SetParameters(settings);
        CHECK(!environment.CaptureCloudInputs());

        GDX12CloudCadence cadence;
        auto displayed = initial;
        unsigned updates = 0, repeats = 0, staleRepeats = 0;
        for (unsigned frame = 0; frame < 120; ++frame)
        {
            const auto displayTime = initial.CapturedAt +
                std::chrono::duration_cast<GDX12CloudEnvironmentInputs::Clock::duration>(
                    std::chrono::duration<double>(frame / 60.0));
            if (cadence.TakeTick(frame / 60.0, 55))
            {
                displayed.CapturedAt = displayTime;
                ++updates;
            }
            else
            {
                ++repeats;
                if (displayed.MeasureAge(displayTime, 15).OverAge) ++staleRepeats;
            }
            CHECK(!cadence.TakeTick(frame / 60.0, 55));
        }
        CHECK(updates == 110 && repeats == 10 && staleRepeats == repeats);
        CHECK(cadence.TakeTick(25, 55) && !cadence.TakeTick(25, 55));
        CHECK(cadence.TakeTick(25, 30) && !cadence.TakeTick(25, 30));
        CHECK(cadence.TakeTick(25, 0));
    }

    std::vector<float> ReadCube(GDX12Device& device, GDX12Texture& texture)
    {
        auto* resource = texture.GetResource();
        const auto desc = resource->D3DResource->GetDesc();
        const UINT count = desc.MipLevels * desc.DepthOrArraySize;
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(count);
        UINT64 bytes = 0;
        device.GetDevice()->GetCopyableFootprints(&desc, 0, count, 0, footprints.data(), nullptr, nullptr, &bytes);
        auto readback = device.CreateResource(D3D12_HEAP_TYPE_READBACK, CD3DX12_RESOURCE_DESC::Buffer(bytes),
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr);
        auto& queue = *device.GetCommandQueue();
        auto* list = queue.GetCommandList();
        list->ResourceBarrier({ resource->GetCopySourceBarrier() });
        for (UINT i = 0; i < count; ++i)
        {
            const CD3DX12_TEXTURE_COPY_LOCATION source(resource->D3DResource.Get(), i);
            const CD3DX12_TEXTURE_COPY_LOCATION target(readback.Get(), footprints[i]);
            list->GetCommandList()->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
        }
        list->ResourceBarrier({ resource->GetSRVBarrier() });
        queue.ExecuteCommandList(list);
        queue.Flush();
        void* mapped = nullptr;
        const D3D12_RANGE read{ 0, static_cast<SIZE_T>(bytes) }, noWrites{ 0, 0 };
        ThrowIfFailed(readback->Map(0, &read, &mapped));
        std::vector<float> values;
        for (const auto& footprint : footprints)
            for (UINT row = 0; row < footprint.Footprint.Height; ++row)
            {
                const auto* data = reinterpret_cast<const uint16_t*>(
                    static_cast<const BYTE*>(mapped) + footprint.Offset + UINT64(row) * footprint.Footprint.RowPitch);
                for (UINT i = 0; i < footprint.Footprint.Width * 4; ++i)
                    values.push_back(DirectX::PackedVector::XMConvertHalfToFloat(data[i]));
            }
        readback->Unmap(0, &noWrites);
        return values;
    }

    void Compare(GDX12Device& device, GDX12EnvironmentIBLPass& environment,
        const std::array<std::vector<float>, 4>& reference, float tolerance, const char* route)
    {
        for (UINT output = 0; output < reference.size(); ++output)
        {
            const auto values = ReadCube(device, *environment.GetOutput(output)->GetTexture());
            CHECK(values.size() == reference[output].size());
            float maximum = 0;
            size_t mismatches = 0;
            for (size_t i = 0; i < values.size(); ++i)
            {
                const auto error = std::abs(values[i] - reference[output][i]);
                if (error > maximum) maximum = error;
                if (!std::isfinite(error) || error > tolerance)
                {
                    if (mismatches++ == 0)
                        std::cerr << route << " output " << output << ", first mismatch at " << i
                            << ": actual=" << values[i] << ", reference=" << reference[output][i] << '\n';
                }
            }
            if (mismatches)
                std::cerr << route << " output " << output << ": " << mismatches << '/' << values.size()
                    << " mismatches, max absolute error=" << maximum << ", tolerance=" << tolerance << '\n';
            CHECK(mismatches == 0);
        }
    }

    void ReportAdapterDifferences(const std::array<std::vector<float>, 4>& primary,
        const std::array<std::vector<float>, 4>& secondary, const char* pixelsPath)
    {
        constexpr float comparisonThreshold = 0.015f;
        std::ofstream pixels;
        if (pixelsPath)
        {
            pixels.open(pixelsPath, std::ios::binary | std::ios::trunc);
            CHECK(pixels);
        }
        for (UINT output : { 0u, 1u, 3u })
        {
            CHECK(primary[output].size() == secondary[output].size());
            std::vector<float> errors;
            errors.reserve(primary[output].size());
            double sum = 0, squared = 0;
            size_t aboveThreshold = 0;
            for (size_t i = 0; i < primary[output].size(); ++i)
            {
                const auto error = std::abs(primary[output][i] - secondary[output][i]);
                CHECK(std::isfinite(error) && primary[output][i] >= 0 && secondary[output][i] >= 0);
                errors.push_back(error);
                if (error > comparisonThreshold) ++aboveThreshold;
                sum += error;
                squared += static_cast<double>(error) * error;
            }
            std::sort(errors.begin(), errors.end());
            const auto p99 = static_cast<size_t>(std::ceil(0.99 * errors.size())) - 1;
            const auto a = std::minmax_element(primary[output].begin(), primary[output].end());
            const auto b = std::minmax_element(secondary[output].begin(), secondary[output].end());
            std::cout << "Per-adapter DIRECT control output " << output
                << ": primary range=[" << *a.first << ',' << *a.second << "], secondary range=["
                << *b.first << ',' << *b.second << "], max absolute error=" << errors.back()
                << ", mean absolute error=" << sum / errors.size()
                << ", RMS=" << std::sqrt(squared / errors.size()) << ", p99=" << errors[p99]
                << ", components above " << comparisonThreshold << '=' << aboveThreshold
                << '/' << primary[output].size() << std::endl;
            if (pixelsPath)
            {
                const uint64_t count = primary[output].size();
                pixels.write(reinterpret_cast<const char*>(&count), sizeof(count));
                pixels.write(reinterpret_cast<const char*>(primary[output].data()), count * sizeof(float));
                pixels.write(reinterpret_cast<const char*>(secondary[output].data()), count * sizeof(float));
                CHECK(pixels);
            }
        }
    }

    CompletionPoint Produce(GDX12CommandQueue& queue, GDX12EnvironmentIBLPass::CloudProducer& producer,
        const GDX12CloudEnvironmentInputs& input, ID3D12Resource& payload)
    {
        auto* list = queue.GetCommandList();
        list->ResourceBarrier({ CD3DX12_RESOURCE_BARRIER::Transition(&payload,
            D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST) });
        producer.Record(list, input, payload);
        list->ResourceBarrier({ CD3DX12_RESOURCE_BARRIER::Transition(&payload,
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON) });
        queue.ExecuteCommandList(list);
        return queue.GetLastCompletionPoint();
    }

    CompletionPoint Display(GDX12CommandQueue& queue, GDX12EnvironmentIBLPass& environment,
        const GDX12CloudEnvironmentInputs& input, const GDX12CloudEnvironmentLayout& layout, ID3D12Resource* payload)
    {
        auto* list = queue.GetCommandList();
        if (payload)
        {
            CHECK(environment.IsCloudDisplayPrepared(layout));
            constexpr UINT indices[] = { 0, 1, 3 };
            std::array<ID3D12Resource*, 3> outputs{};
            for (size_t i = 0; i < outputs.size(); ++i)
            {
                auto* resource = environment.GetOutput(indices[i])->GetTexture()->GetResource();
                list->ResourceBarrier({ resource->GetCopyDestBarrier() });
                outputs[i] = resource->D3DResource.Get();
            }
            list->ResourceBarrier({ CD3DX12_RESOURCE_BARRIER::Transition(payload,
                D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE) });
            layout.RecordUnpack(list, outputs, *payload);
            list->ResourceBarrier({ CD3DX12_RESOURCE_BARRIER::Transition(payload,
                D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON) });
            for (const auto index : indices)
                list->ResourceBarrier({ environment.GetOutput(index)->GetTexture()->GetResource()->GetSRVBarrier() });
        }
        else
            environment.RepeatCloudSnapshot(list, input);
        queue.ExecuteCommandList(list);
        if (payload) environment.PublishCloudSnapshot(input);
        return queue.GetLastCompletionPoint();
    }

    bool SameLuid(LUID a, LUID b) { return a.LowPart == b.LowPart && a.HighPart == b.HighPart; }

    ComPtr<IDXGIAdapter4> SecondAdapter(LUID first)
    {
        for (UINT i = 0;; ++i)
        {
            ComPtr<IDXGIAdapter4> adapter;
            const auto result = GDX12DeviceFactory::GetFactory()->EnumAdapterByGpuPreference(i,
                DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
            if (result == DXGI_ERROR_NOT_FOUND) break;
            ThrowIfFailed(result);
            DXGI_ADAPTER_DESC1 desc{};
            ThrowIfFailed(adapter->GetDesc1(&desc));
            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE || SameLuid(first, desc.AdapterLuid)) continue;
            const auto supported = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device14), nullptr);
            if (supported == DXGI_ERROR_UNSUPPORTED || supported == E_NOTIMPL) continue;
            ThrowIfFailed(supported);
            return adapter;
        }
        throw std::runtime_error("Cloud placement requires a second compatible hardware adapter");
    }

    bool CheckDebug(ID3D12InfoQueue& info)
    {
        bool clean = true;
        for (UINT64 i = 0; i < info.GetNumStoredMessages(); ++i)
        {
            SIZE_T size = 0;
            ThrowIfFailed(info.GetMessage(i, nullptr, &size));
            std::vector<BYTE> storage(size);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            ThrowIfFailed(info.GetMessage(i, message, &size));
            if (message->Severity > D3D12_MESSAGE_SEVERITY_ERROR) continue;
            std::cerr << message->pDescription << '\n';
            clean = false;
        }
        return clean;
    }

    struct Gate
    {
        ComPtr<ID3D12Fence> Fence;
        Gate(GDX12Device& device, GDX12CommandQueue& queue)
        {
            ThrowIfFailed(device.GetDevice()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&Fence)));
            ThrowIfFailed(queue.GetCommandQueue()->Wait(Fence.Get(), 1));
        }
        ~Gate() { if (Fence) Fence->Signal(1); }
        bool Closed() const { return Fence && Fence->GetCompletedValue() == 0; }
        void Open()
        {
            ThrowIfFailed(Fence->Signal(1));
            Fence.Reset();
        }
    };

    void WaitForFixture(GDX12CommandQueue& queue, CompletionPoint point, const char* operation)
    {
        CHECK(point.IsValid() && point.Timeline() == queue.GetTimelineIdentity());
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
        for (;;)
        {
            const auto progress = queue.GetCompletionProgress();
            if (point.IsReachedBy(progress)) return;
            if (progress.CompletedValue == UINT64_MAX) ThrowIfFailed(DXGI_ERROR_DEVICE_REMOVED);
            if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error(operation);
            Sleep(1);
        }
    }

    void WaitForFixture(GDX12CommandQueue& queue, const char* operation)
    {
        WaitForFixture(queue, queue.GetLastCompletionPoint(), operation);
    }

    std::vector<std::byte> ReadPacked(GDX12Device& device, GDX12CommandQueue& queue, ID3D12Resource& payload)
    {
        const auto desc = payload.GetDesc();
        CHECK(desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER);
        auto readback = device.CreateResource(D3D12_HEAP_TYPE_READBACK,
            CD3DX12_RESOURCE_DESC::Buffer(desc.Width), D3D12_RESOURCE_STATE_COPY_DEST);
        auto* list = queue.GetCommandList();
        list->ResourceBarrier({ CD3DX12_RESOURCE_BARRIER::Transition(&payload,
            D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE) });
        list->GetCommandList()->CopyBufferRegion(readback.Get(), 0, &payload, 0, desc.Width);
        list->ResourceBarrier({ CD3DX12_RESOURCE_BARRIER::Transition(&payload,
            D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON) });
        queue.ExecuteCommandList(list);
        WaitForFixture(queue, "Packed cloud readback did not retire");
        std::vector<std::byte> bytes(static_cast<size_t>(desc.Width));
        const D3D12_RANGE read{ 0, bytes.size() }, noWrites{ 0, 0 };
        void* mapped = nullptr;
        ThrowIfFailed(readback->Map(0, &read, &mapped));
        std::memcpy(bytes.data(), mapped, bytes.size());
        readback->Unmap(0, &noWrites);
        return bytes;
    }

    class CapacityBatch final : public GDX12RenderPass
    {
    public:
        explicit CapacityBatch(Placement placement) : _placement(placement) {}
        bool SupportsGraphQueue(QueueKind queue) const override { return queue == _placement.Queue; }
        void RecordPlacedCommands(GDX12CommandList*, const GDX12RenderGraph::BoundResources&) override {}
        void RecordRenderGraph(GDX12RenderGraphBuilder& graph) override
        {
            graph.SetName("Cloud fixture capacity");
            graph.SetPlacement(_placement);
            graph.DeclareCompleteAccesses();
            graph.HasSideEffects();
        }

    private:
        Placement _placement;
    };

    void CheckController(GDX12Device& device, GDX12Device* secondary, RenderMGPURuntime* runtime,
        GDX12CommandQueue* primaryCopy = nullptr, GDX12CommandQueue* secondaryCopy = nullptr)
    {
        ConsoleModule console;
        const auto set = [&](const wchar_t* name, const char* value)
        {
            CHECK(console.FindConsoleVariable(name).SetFromString(value));
        };
        using Route = CrossAdapterEnvironment::Route;
        const auto route = secondary ? Route::SecondaryCompute : Route::LocalCompute;
        set(L"Render.Clouds.Bistro", "1");
        set(L"Render.Clouds.AsyncCompute", secondary ? "0" : "1");
        set(L"Render.Clouds.MultiGPU", secondary ? "1" : "0");
        set(L"Render.Clouds.Latest", "0");
        GDX12DeviceResources resources;
        resources.Initialize(&device);
        resources.Sky.Box.IBL = { 32, 16, 1 };
        RenderPipelineCommonData common;
        GDX12EnvironmentIBLPass environment;
        environment.Setup(&resources, nullptr, &common);
        environment.Initialize();
        CrossAdapterEnvironment controller(environment, resources, secondary, runtime);
        GDX12RenderGraph graph(&device);
        graph.ImportShared(GDX12RenderGraph::SharedResource::Scene, "Fixture scene");
        graph.AddPass(controller);
        graph.SetLegacySplit(controller, {},
            { controller.GetOutput(0), controller.GetOutput(1), controller.GetOutput(2), controller.GetOutput(3) });
        graph.Compile();
        auto* direct = device.GetCommandQueue();
        const auto render = [&](const double delta)
        {
            if (runtime) runtime->Poll();
            const bool routed = controller.PrepareFrame(delta);
            auto* commands = direct->GetCommandList();
            if (routed)
                commands = graph.ExecuteLegacySplit(commands, nullptr,
                    [&](const auto& boundary, const auto prefix) { return controller.Submit(boundary, prefix); });
            else commands = graph.Execute(commands);
            direct->ExecuteCommandList(commands);
            WaitForFixture(*direct, "Primary DIRECT failed to complete cloud frame");
        };
        render(0.1);
        CHECK(controller.GetDiagnostics().Effective == Route::Direct && controller.GetDiagnostics().Requested == route);
        render(0.1);
        CHECK(controller.GetDiagnostics().Effective == route);
        CHECK(controller.GetDiagnostics().Submitted == 1 && controller.GetDiagnostics().DisplayedInvocation == 1);
        render(0);
        CHECK(controller.GetDiagnostics().Submitted == 1 && controller.GetDiagnostics().Repeats == 1);

        auto& producer = secondary ? *secondary : device;
        auto* compute = producer.TryGetComputeQueue();
        CHECK(compute);
        if (secondary)
        {
            CHECK(runtime && primaryCopy && secondaryCopy);
            CapacityBatch capacity({ secondary->GetSubmissionAdapterId(), QueueKind::Copy });
            GDX12RenderGraph occupied;
            occupied.AddPass(capacity);
            occupied.CompilePlan();
            Gate holdCapacity(*secondary, *secondaryCopy);
            bool exhausted = false;
            for (unsigned i = 0; i < 64; ++i)
            {
                if (!runtime->TrySubmit(occupied, {}))
                {
                    exhausted = true;
                    break;
                }
            }
            CHECK(exhausted && runtime->PendingCount() > 0);
            const auto before = controller.GetDiagnostics();
            const auto computePoint = compute->GetLastCompletionPoint().Value();
            const auto exportPoint = secondaryCopy->GetLastCompletionPoint().Value();
            const auto importPoint = primaryCopy->GetLastCompletionPoint().Value();
            render(0.1);
            const auto fallback = controller.GetDiagnostics();
            CHECK(holdCapacity.Closed());
            CHECK(fallback.Effective == Route::Direct && fallback.ExactDeferred == before.ExactDeferred + 1);
            CHECK(fallback.Submitted == before.Submitted && fallback.DisplayedInvocation == 0);
            CHECK(compute->GetLastCompletionPoint().Value() == computePoint);
            CHECK(secondaryCopy->GetLastCompletionPoint().Value() == exportPoint);
            CHECK(primaryCopy->GetLastCompletionPoint().Value() == importPoint);
            holdCapacity.Open();
            WaitForFixture(*secondaryCopy, "Cloud capacity work did not retire");
            render(0.1);
            const auto recovered = controller.GetDiagnostics();
            CHECK(recovered.Effective == route && recovered.Submitted == before.Submitted + 1);
            CHECK(recovered.DisplayedInvocation > before.DisplayedInvocation);
            std::cout << "Cloud Exact no-submit capacity fallback and recovery passed." << std::endl;
        }

        set(L"Render.Clouds.Latest", "1");
        set(L"Render.Clouds.MaxAgeMs", "0.0001");
        Gate gate(producer, *compute);
        render(0.1);
        CHECK(controller.GetDiagnostics().Effective == Route::Direct);
        const auto pending = controller.GetDiagnostics().Submitted;
        render(0);
        CHECK(controller.GetDiagnostics().Effective == Route::Direct);
        CHECK(controller.GetDiagnostics().Submitted == pending);
        CHECK(gate.Closed());
        gate.Open();
        WaitForFixture(*compute, "Cloud Latest producer did not retire after gate release");
        if (secondary)
        {
            Gate holdExport(*secondary, *secondaryCopy);
            render(0);
            const auto primaryMarker = primaryCopy->SignalCompletion();
            WaitForFixture(*primaryCopy, primaryMarker, "Primary COPY waited for an unfinished cloud export");
            render(0);
            CHECK(holdExport.Closed() && controller.GetDiagnostics().Effective == Route::Direct);
            holdExport.Open();
            WaitForFixture(*secondaryCopy, "Cloud export did not retire after gate release");
            Gate holdImport(device, *primaryCopy);
            const auto importBefore = primaryCopy->GetLastCompletionPoint().Value();
            render(0);
            CHECK(primaryCopy->GetLastCompletionPoint().Value() > importBefore);
            render(0);
            CHECK(holdImport.Closed() && controller.GetDiagnostics().Effective == Route::Direct);
            holdImport.Open();
            WaitForFixture(*primaryCopy, "Cloud import did not retire after gate release");
        }
        render(0);
        CHECK(controller.GetDiagnostics().Effective == route);
        const auto firstDisplay = controller.GetDiagnostics();
        CHECK(firstDisplay.DisplayAge.OverAge && firstDisplay.OverAgeCrossings == 1);
        render(0);
        const auto repeat = controller.GetDiagnostics();
        CHECK(repeat.Submitted == firstDisplay.Submitted && repeat.DisplayedInvocation == firstDisplay.DisplayedInvocation);
        CHECK(repeat.Repeats == firstDisplay.Repeats + 1 && repeat.OverAgeCrossings == 1);
        CHECK(repeat.DisplayAge.Milliseconds >= firstDisplay.DisplayAge.Milliseconds);
        if (secondary)
        {
            const auto submittedBefore = controller.GetDiagnostics().Submitted;
            for (unsigned i = 0; i < 6; ++i)
            {
                render(0.05);
                WaitForFixture(*compute, "Cloud pipelined producer did not retire");
                WaitForFixture(*secondaryCopy, "Cloud pipelined export did not retire");
                WaitForFixture(*primaryCopy, "Cloud pipelined import did not retire");
            }
            CHECK(controller.GetDiagnostics().Submitted == submittedBefore + 6);
            Gate holdResize(*secondary, *compute);
            render(0.1);
            CHECK(controller.GetDiagnostics().Effective == route);
            const auto oldEpoch = controller.GetDiagnostics();
            resources.Sky.Box.IBL.CubeResolution = 48;
            controller.Resize();
            render(0);
            CHECK(controller.GetDiagnostics().Effective == Route::Direct);
            CHECK(holdResize.Closed() && controller.GetDiagnostics().DisplayedInvocation == 0);
            CHECK(controller.GetDiagnostics().EpochResets == oldEpoch.EpochResets + 1);
            CHECK(controller.GetOutput(3)->GetTexture()->GetWidth() == 48);
            holdResize.Open();
            WaitForFixture(*compute, "Retired cloud producer did not complete after resize");
            for (unsigned i = 0; i < 8 && controller.GetDiagnostics().DisplayedInvocation == 0; ++i)
            {
                render(0);
                WaitForFixture(*compute, "Resized cloud producer did not retire");
                WaitForFixture(*secondaryCopy, "Resized cloud export did not retire");
                WaitForFixture(*primaryCopy, "Resized cloud import did not retire");
            }
            const auto resized = controller.GetDiagnostics();
            CHECK(resized.Effective == route && resized.EpochResets == oldEpoch.EpochResets + 1);
            CHECK(resized.DisplayedInvocation > oldEpoch.DisplayedInvocation);
            CHECK(resized.Submitted == oldEpoch.Submitted + 1);
            CHECK(controller.GetOutput(3)->GetTexture()->GetWidth() == 48);
            std::cout << "Cloud Latest completed-only reads and resized epoch publication passed." << std::endl;

            set(L"Render.Clouds.Latest", "0");
            set(L"Render.Clouds.MultiGPU", "0");
            render(0.1);
            CHECK(controller.GetDiagnostics().Effective == Route::Direct);
            set(L"Render.Clouds.AsyncCompute", "1");
            render(0.1);
            CHECK(controller.GetDiagnostics().Effective == Route::LocalCompute);
            set(L"Render.Clouds.MultiGPU", "1");
            render(0.1);
            CHECK(controller.GetDiagnostics().Effective == Route::SecondaryCompute);
            set(L"Render.Clouds.MultiGPU", "0");
            set(L"Render.Clouds.AsyncCompute", "0");
            render(0.1);
            CHECK(controller.GetDiagnostics().Effective == Route::Direct);
            std::cout << "Cloud live DIRECT / local / secondary / DIRECT route switches passed." << std::endl;
        }
        set(L"Render.Clouds.Bistro", "0");
        set(L"Render.Clouds.AsyncCompute", "0");
        set(L"Render.Clouds.MultiGPU", "0");
        set(L"Render.Clouds.MaxAgeMs", "0");
    }

    void CheckPlacement(GDX12Device& primary, GDX12Device& secondary, const char* pixelsPath)
    {
        GDX12DeviceResources primaryResources, secondaryResources;
        primaryResources.Initialize(&primary);
        secondaryResources.Initialize(&secondary);
        primaryResources.Sky.Box.IBL = { 32, 16, 1 };
        RenderPipelineCommonData common;
        GDX12CloudComponent clouds;
        GDX12EnvironmentIBLPass environment;
        environment.Setup(&primaryResources, nullptr, &common);
        environment.SetCloudSource(&clouds);
        environment.Initialize();
        clouds.Advance(0.25);
        const auto input = environment.CaptureCloudInputs().value();
        const GDX12CloudEnvironmentLayout layout(*primary.GetDevice().Get(), input.IBL.CubeResolution);
        const auto desc = layout.BufferDesc();
        std::cout << "Frozen cloud input: time=" << input.Cloud.SimulationSeconds << ", phase=("
            << input.Cloud.Phase.x << ',' << input.Cloud.Phase.y << "), seed=" << input.Cloud.Parameters.Seed
            << ", quality=" << input.Cloud.Quality << ", octaves=" << input.Cloud.Octaves
            << ", sun=(" << input.SunDirection.x << ',' << input.SunDirection.y << ',' << input.SunDirection.z
            << "), radiance=(" << input.SunRadiance.x << ',' << input.SunRadiance.y << ',' << input.SunRadiance.z
            << "), IBL cube=" << input.IBL.CubeResolution << ", samples=" << input.IBL.FilterSamples
            << ", intensity=" << input.IBL.Intensity << ", packed bytes=" << desc.Width << std::endl;
        auto& direct = *primary.GetCommandQueue();
        auto* commands = direct.GetCommandList();
        environment.Execute(commands);
        direct.ExecuteCommandList(commands);
        direct.Flush();

        std::array<std::vector<float>, 4> reference;
        for (UINT output = 0; output < reference.size(); ++output)
            reference[output] = ReadCube(primary, *environment.GetOutput(output)->GetTexture());
        CHECK(reference[3].size() == 6 * 4 * (32 * 32 + 16 * 16 + 8 * 8 + 4 * 4 + 2 * 2 + 1));
        CHECK(reference[3][0] > 0 && reference[2][0] > 0);
        const auto revision = primaryResources.ReflectionEnvironmentRevision;
        const auto* dfg = environment.GetOutput(2)->GetTexture()->GetResource();

        GDX12CommandQueue localCompute(&primary, D3D12_COMMAND_LIST_TYPE_COMPUTE);
        GDX12EnvironmentIBLPass::CloudProducer local(primaryResources, layout);
        const auto localPayload = primary.CreateResource(D3D12_HEAP_TYPE_DEFAULT, desc, D3D12_RESOURCE_STATE_COMMON, nullptr);
        Produce(direct, local, input, *localPayload.Get());
        Display(direct, environment, input, layout, localPayload.Get());
        Compare(primary, environment, reference, 0, "Primary DIRECT producer");
        direct.WaitFor(Produce(localCompute, local, input, *localPayload.Get()));
        Display(direct, environment, input, layout, localPayload.Get());
        Compare(primary, environment, reference, 0, "Local COMPUTE");
        std::cout << "Local COMPUTE matched primary DIRECT across every output component." << std::endl;
        Display(direct, environment, input, layout, nullptr);
        direct.Flush();
        CHECK(primaryResources.ReflectionEnvironmentRevision == revision);
        CHECK(environment.GetOutput(2)->GetTexture()->GetResource() == dfg);

        secondaryResources.Sky = primaryResources.Sky;
        secondaryResources.Lighting = primaryResources.Lighting;
        GDX12CloudComponent controlClouds;
        controlClouds.SetParameters(input.Cloud.Parameters);
        controlClouds.Advance(input.Cloud.SimulationSeconds);
        GDX12EnvironmentIBLPass secondaryControl;
        secondaryControl.Setup(&secondaryResources, nullptr, &common);
        secondaryControl.SetCloudSource(&controlClouds);
        secondaryControl.Initialize();
        const auto control = secondaryControl.CaptureCloudInputs().value();
        CHECK(control.Cloud.Phase == input.Cloud.Phase && control.Cloud.SimulationSeconds == input.Cloud.SimulationSeconds);
        CHECK(control.Cloud.Quality == input.Cloud.Quality && control.Cloud.Octaves == input.Cloud.Octaves);
        CHECK(control.Cloud.Parameters.Seed == input.Cloud.Parameters.Seed && control.Cloud.Enabled == input.Cloud.Enabled);
        CHECK(control.SunDirection == input.SunDirection && control.SunRadiance == input.SunRadiance);
        CHECK(control.IBL.CubeResolution == input.IBL.CubeResolution && control.IBL.FilterSamples == input.IBL.FilterSamples &&
            control.IBL.Intensity == input.IBL.Intensity);
        const GDX12CloudEnvironmentLayout secondaryLayout(*secondary.GetDevice().Get(), input.IBL.CubeResolution);
        CHECK(secondaryLayout.BufferDesc().Width == desc.Width);
        auto& controlDirect = *secondary.GetCommandQueue();
        commands = controlDirect.GetCommandList();
        secondaryControl.Execute(commands);
        controlDirect.ExecuteCommandList(commands);
        WaitForFixture(controlDirect, "Secondary DIRECT control did not retire");
        std::array<std::vector<float>, 4> remoteReference;
        for (const auto output : { 0u, 1u, 3u })
            remoteReference[output] = ReadCube(secondary, *secondaryControl.GetOutput(output)->GetTexture());
        ReportAdapterDifferences(reference, remoteReference, pixelsPath);
        remoteReference[2] = reference[2];

        GDX12CommandQueue remoteCompute(&secondary, D3D12_COMMAND_LIST_TYPE_COMPUTE);
        GDX12CommandQueue exportQueue(&secondary, D3D12_COMMAND_LIST_TYPE_COPY, true);
        GDX12CommandQueue importQueue(&primary, D3D12_COMMAND_LIST_TYPE_COPY);
        GDX12EnvironmentIBLPass::CloudProducer remote(secondaryResources, layout);
        const auto remotePayload = secondary.CreateResource(D3D12_HEAP_TYPE_DEFAULT, desc, D3D12_RESOURCE_STATE_COMMON, nullptr);
        PoolIds ids{};
        for (size_t i = 0; i < ids.size(); ++i)
        {
            const uint64_t id = 16 * (i + 1);
            ids[i] = { ResourceId{id}, ResourceId{id + 1}, ResourceId{id + 2}, AllocationId{id + 3}, AllocationId{id + 4} };
        }
        constexpr TransferId stream{ 1 };
        constexpr Epoch epoch{ 1 };
        Transport bank(secondary, primary, exportQueue, importQueue, stream, epoch, desc, ids);
        uint64_t invocation = 0;
        for (const auto mode : { SnapshotReadMode::ExactVersion, SnapshotReadMode::LatestCompleted })
        {
            const TransferVersion version{ stream, epoch, ProducerInvocationId{ ++invocation } };
            const auto credit = bank.Reserve(version, mode, remoteCompute, remotePayload, AllocationId{ 2 });
            CHECK(credit.Status == Result::Accepted);
            CHECK(bank.BindProducer(credit.Publication, Produce(remoteCompute, remote, input, *remotePayload.Get())));
            const auto sourceBytes = ReadPacked(secondary, remoteCompute, *remotePayload.Get());
            bank.Poll();
            CHECK(bank.SubmitExport(credit.Publication).Status == Result::Accepted);
            if (mode == SnapshotReadMode::LatestCompleted)
            {
                exportQueue.Flush();
                bank.Poll();
                const auto beforeImport = bank.SelectLatest(FrameSubmissionId{ 1 }).Current;
                CHECK(!beforeImport.IsValid() || !(beforeImport.Version == version));
            }
            CHECK(bank.SubmitImport(credit.Publication).Status == Result::Accepted);
            if (mode == SnapshotReadMode::LatestCompleted)
            {
                importQueue.Flush();
                bank.Poll();
                const auto selected = bank.SelectLatest(FrameSubmissionId{ 2 }).Current;
                CHECK(selected.IsValid() && selected.Version == version);
                CHECK(bank.SelectLatest(FrameSubmissionId{ 3 }).Current.Version == version);
            }
            const auto reader = bank.AcquireReader(credit.Publication, direct);
            CHECK(reader && reader->Lease.View.Version == version);
            direct.WaitFor(reader->Lease.View.ImportReady);
            CHECK(ReadPacked(primary, direct, *reader->View.Resource->D3DResource.Get()) == sourceBytes);
            CHECK(bank.BindReader(reader->Lease.Id, Display(direct, environment, input, layout,
                reader->View.Resource->D3DResource.Get())));
            if (mode == SnapshotReadMode::ExactVersion) CHECK(bank.ReleaseExact(credit.Publication));
            Compare(primary, environment, remoteReference, 0,
                mode == SnapshotReadMode::ExactVersion ? "Secondary Exact" : "Secondary Latest");
            std::cout << (mode == SnapshotReadMode::ExactVersion ? "Secondary Exact" : "Secondary Latest")
                << " matched secondary DIRECT and preserved all " << sourceBytes.size() << " packed bytes." << std::endl;
            bank.Poll();
            CHECK(bank.SourceReusable(credit.Publication));
        }
        CHECK(bank.Reset(Epoch{ 2 }));
        CHECK(!bank.SelectLatest(FrameSubmissionId{ 4 }).Current.IsValid());
        clouds.Reset();
        auto seeded = clouds.GetParameters();
        seeded.Seed = 23;
        clouds.SetParameters(seeded);
        primaryResources.Sky.Box.IBL.CubeResolution = 48;
        environment.Resize();
        const auto resized = environment.CaptureCloudInputs().value();
        CHECK(!resized.IsCompatibleWith(input) && resized.Cloud.SimulationSeconds == 0);
        const GDX12CloudEnvironmentLayout resizedLayout(*primary.GetDevice().Get(), resized.IBL.CubeResolution);
        for (auto& id : ids)
        {
            id.Export = ResourceId{ 1000 + static_cast<uint64_t>(id.Export) };
            id.Import = ResourceId{ 1000 + static_cast<uint64_t>(id.Import) };
            id.Local = ResourceId{ 1000 + static_cast<uint64_t>(id.Local) };
            id.SharedBacking = AllocationId{ 1000 + static_cast<uint64_t>(id.SharedBacking) };
            id.LocalBacking = AllocationId{ 1000 + static_cast<uint64_t>(id.LocalBacking) };
        }
        bank.Poll();
        CHECK(bank.TryReconfigure(resizedLayout.BufferDesc(), ids));
        CHECK(!environment.IsCloudDisplayPrepared(resizedLayout));
        commands = direct.GetCommandList();
        environment.Execute(commands);
        direct.ExecuteCommandList(commands);
        direct.Flush();
        CHECK(environment.IsCloudDisplayPrepared(resizedLayout));
        CHECK(environment.GetOutput(2)->GetTexture()->GetResource() == dfg);
        bank.Shutdown();

        environment.SetCloudSource(nullptr);
        commands = direct.GetCommandList();
        environment.Execute(commands);
        direct.ExecuteCommandList(commands);
        direct.Flush();
        for (const auto output : { 0u, 1u, 3u })
        {
            const auto values = ReadCube(primary, *environment.GetOutput(output)->GetTexture());
            CHECK(values.size() == 6 * 4);
            for (size_t i = 0; i < values.size(); ++i)
                CHECK(values[i] == (i % 4 == 3 ? 1.0f : 0.0f));
        }
        CHECK(environment.GetOutput(2)->GetTexture()->GetResource() == dfg);
        CHECK(ReadCube(primary, *environment.GetOutput(2)->GetTexture()) == reference[2]);
        std::cout << "Cloud-to-Off reset preserved opaque black on all 18 faces and the device-local DFG."
            << std::endl;
    }
}

int main(int argc, char** argv)
{
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    try
    {
        CheckInputsAndCadence();
        if (argc == 1)
        {
            std::cout << "Cloud immutable inputs and 55 Hz / 60 Hz cadence checks passed.\n";
            return 0;
        }
        const std::string_view mode = argv[1];
        CHECK(((argc == 2 || argc == 3) && mode == "--gpu-validation") ||
            (argc == 2 && mode == "--controller-gbv"));
        TestApp app;
        ComPtr<ID3D12Debug1> debug;
        ThrowIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
        debug->EnableDebugLayer();
        debug->SetEnableGPUBasedValidation(TRUE);
        GDX12Device primary, secondary;
        ThrowIfFailed(primary.Initialize(GDX12DeviceFactory::GetMostPerformantAdapter()));
        secondary.Role = DEVICE_ROLE_SECONDARY;
        ThrowIfFailed(secondary.Initialize(SecondAdapter(primary.GetAdapterLuid())));
        const auto first = primary.GetAdapterLuid(), second = secondary.GetAdapterLuid();
        std::cout << "Physical LUIDs " << first.HighPart << ':' << first.LowPart
            << " and " << second.HighPart << ':' << second.LowPart << '\n';
        ComPtr<ID3D12InfoQueue> primaryInfo, secondaryInfo;
        ThrowIfFailed(primary.GetDevice().As(&primaryInfo));
        ThrowIfFailed(secondary.GetDevice().As(&secondaryInfo));
        try
        {
            if (mode == "--gpu-validation") CheckPlacement(primary, secondary, argc == 3 ? argv[2] : nullptr);
            CheckController(primary, nullptr, nullptr);
            GDX12CommandQueue primaryCopy(&primary, D3D12_COMMAND_LIST_TYPE_COPY, true);
            GDX12CommandQueue secondaryCopy(&secondary, D3D12_COMMAND_LIST_TYPE_COPY, true);
            RenderMGPURuntime runtime({ &primary, &secondary, &primaryCopy, &secondaryCopy });
            CheckController(primary, &secondary, &runtime, &primaryCopy, &secondaryCopy);
            runtime.Shutdown();
        }
        catch (...)
        {
            CheckDebug(*primaryInfo.Get());
            CheckDebug(*secondaryInfo.Get());
            throw;
        }
        ThrowIfFailed(primary.GetDevice()->GetDeviceRemovedReason());
        ThrowIfFailed(secondary.GetDevice()->GetDeviceRemovedReason());
        const bool primaryClean = CheckDebug(*primaryInfo.Get());
        const bool secondaryClean = CheckDebug(*secondaryInfo.Get());
        CHECK(primaryClean && secondaryClean);
        GDX12ShaderCompiler::Shutdown();
        if (mode == "--gpu-validation")
            std::cout << "Cloud cube and IBL subresources passed DIRECT, local COMPUTE and two-adapter Exact/Latest checks.\n";
        else std::cout << "Cloud controller lifecycle checks passed on one and two adapters.\n";
        return 0;
    }
    catch (const DxException& error) { std::wcerr << error.ToString() << L'\n'; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
    return 1;
}
