// Cross-adapter transport and equal-step production solver checks; no scene assets.
#include "Rendering/Water/GDX12WaterOffload.h"
#include "Engine.RendererDX12/GDX12CommandList.h"
#include "Engine.RendererDX12/GDX12CommandQueue.h"
#include "Engine.RendererDX12/GDX12DeviceFactory.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"
#include <d3d12sdklayers.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
    using Particle = GDX12FluidSolver::Particle;

    void Require(bool condition, const char* message)
    {
        if (!condition) throw std::runtime_error(message);
    }

    void DebugErrors(ID3D12Device* device)
    {
        ComPtr<ID3D12InfoQueue> info;
        ThrowIfFailed(device->QueryInterface(IID_PPV_ARGS(&info)));
        for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i)
        {
            SIZE_T bytes = 0;
            ThrowIfFailed(info->GetMessage(i, nullptr, &bytes));
            std::vector<char> storage(bytes);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            ThrowIfFailed(info->GetMessage(i, message, &bytes));
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR)
                throw std::runtime_error(message->pDescription);
        }
        info->ClearStoredMessages();
    }

    class Fixture
    {
    public:
        GDX12Device Device;
        std::unique_ptr<GDX12DescriptorHeap> Heap;
        GDX12FluidSolver Solver;

        explicit Fixture(ComPtr<IDXGIAdapter4> adapter)
        {
            ThrowIfFailed(Device.Initialize(adapter));
            Heap = std::make_unique<GDX12DescriptorHeap>(&Device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                4096, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
            Solver.Initialize(&Device);
        }

        ~Fixture() { Device.GetCommandQueue()->Flush(); }

        template<class T> std::vector<T> Read(GDX12UploadBuffer<T>& source)
        {
            auto& resource = source.GetResource();
            const auto bytes = resource.D3DResource->GetDesc().Width;
            auto readback = Device.CreateResource(D3D12_HEAP_TYPE_READBACK,
                CD3DX12_RESOURCE_DESC::Buffer(bytes), D3D12_RESOURCE_STATE_COPY_DEST);
            auto* queue = Device.GetCommandQueue();
            auto* commands = queue->GetCommandList();
            const auto prior = resource.GetCurrentState();
            commands->ResourceBarrier({resource.GetCopySourceBarrier()});
            commands->CopyResource(readback.Get(), resource.D3DResource.Get());
            commands->ResourceBarrier({resource.GetTransitionBarrier(prior)});
            queue->ExecuteCommandList(commands);
            queue->Flush();
            void* data = nullptr;
            ThrowIfFailed(readback->Map(0, nullptr, &data));
            std::vector<T> result(static_cast<size_t>(bytes) / sizeof(T));
            std::memcpy(result.data(), data, static_cast<size_t>(bytes));
            readback->Unmap(0, nullptr);
            return result;
        }

        void Advance(GDX12FluidSolver::Buffers& buffers, GDX12FluidSolver::Head& head,
            GDX12UploadBuffer<GDX12FluidParameters>& constants,
            const GDX12FluidInstance& input, GDX12FluidSolver::Steps steps)
        {
            auto* queue = Device.GetCommandQueue();
            queue->Flush();
            constants.CopyData(0, MakeFluidParameters(input.ContainerToWorld, input.Component.GetSettings()));
            auto* commands = queue->GetCommandList();
            commands->SetDescriptorHeaps({Heap.get()});
            head = Solver.Record(*commands, buffers, head, input.Component.GetSettings(), steps,
                constants.GetElementAddress(0));
            queue->ExecuteCommandList(commands);
            queue->Flush();
        }
    };

    void ValidateLocalKernels(Fixture& fixture)
    {
        constexpr UINT count = 257;
        std::vector<Particle> initial(count);
        std::vector<XMFLOAT3> incoming(count);
        for (UINT i = 0; i < count; ++i)
        {
            initial[i] = {{float(i) * .125f, -float(i), .25f}, {1, 2, 3}, {-10, -20, -30}};
            incoming[i] = {float(i) + .5f, float(i) * .25f, -float(i) * .5f};
        }
        GDX12UploadBuffer<Particle> upload(&fixture.Device, count, EBufferType::Upload, false);
        GDX12UploadBuffer<XMFLOAT3> uploadIncoming(&fixture.Device, count, EBufferType::Upload, false);
        upload.CopyData(0, initial.data(), count);
        uploadIncoming.CopyData(0, incoming.data(), count);
        GDX12UploadBuffer<Particle> solver(&fixture.Device, count, EBufferType::Default, false);
        GDX12UploadBuffer<Particle> display(&fixture.Device, count, EBufferType::Default, false);
        GDX12UploadBuffer<XMFLOAT3> packed(&fixture.Device, count, EBufferType::Default, false);
        const auto descriptors = [&](auto& buffer)
        {
            buffer.CreateSRV(fixture.Heap.get(), fixture.Heap->GetAvailableIndex());
            buffer.CreateUAV(fixture.Heap.get(), fixture.Heap->GetAvailableIndex());
        };
        descriptors(solver); descriptors(display); descriptors(packed);
        GDX12RootSignatureDesc desc;
        desc.VolatileSRVData = true;
        desc.Constants = {2}; desc.NumSingleSRVSlots = 2; desc.NumSingleUAVSlots = 2;
        GDX12RootSignature root(&fixture.Device, desc);
        const auto pack = GDX12RenderHelpers::CreateComputePipeline(&fixture.Device, &root,
            APP_SHADERS_FOLDER "WaterTransfer.hlsl", "Pack");
        const auto advance = GDX12RenderHelpers::CreateComputePipeline(&fixture.Device, &root,
            APP_SHADERS_FOLDER "WaterTransfer.hlsl", "AdvanceDisplay");
        auto* queue = fixture.Device.GetCommandQueue();
        auto* commands = queue->GetCommandList();
        commands->ResourceBarrier({solver.GetResource().GetCopyDestBarrier(), display.GetResource().GetCopyDestBarrier()});
        commands->CopyResource(solver.GetResource().D3DResource.Get(), upload.GetResource().D3DResource.Get());
        commands->CopyResource(display.GetResource().D3DResource.Get(), upload.GetResource().D3DResource.Get());
        commands->ResourceBarrier({solver.GetResource().GetNonPixelShaderResourceBarrier(), packed.GetResource().GetUnorderedAccessBarrier()});
        commands->SetDescriptorHeaps({fixture.Heap.get()});
        commands->SetComputeRootSignature(&root);
        const UINT constants[] = {count, 1};
        commands->GetCommandList()->SetComputeRoot32BitConstants(0, 2, constants, 0);
        commands->SetComputeSRV(0, solver.GetSRV()->GPUHandle);
        commands->SetComputeUAV(0, packed.GetUAV()->GPUHandle);
        commands->SetPipelineState(pack);
        commands->Dispatch((count + 127) / 128, 1, 1);
        queue->ExecuteCommandList(commands); queue->Flush();
        const auto positions = fixture.Read(packed);
        for (UINT i = 0; i < count; ++i)
            Require(std::memcmp(&positions[i], &initial[i].Position, sizeof(XMFLOAT3)) == 0,
                "Local Pack changed position bytes");
        for (UINT hasNew : {1u, 0u})
        {
            commands = queue->GetCommandList();
            if (hasNew)
            {
                commands->ResourceBarrier({packed.GetResource().GetCopyDestBarrier()});
                commands->CopyResource(packed.GetResource().D3DResource.Get(), uploadIncoming.GetResource().D3DResource.Get());
            }
            commands->ResourceBarrier({packed.GetResource().GetNonPixelShaderResourceBarrier(), display.GetResource().GetUnorderedAccessBarrier()});
            commands->SetDescriptorHeaps({fixture.Heap.get()});
            commands->SetComputeRootSignature(&root);
            const UINT dispatch[] = {count, hasNew};
            commands->GetCommandList()->SetComputeRoot32BitConstants(0, 2, dispatch, 0);
            commands->SetComputeSRV(1, packed.GetSRV()->GPUHandle);
            commands->SetComputeUAV(1, display.GetUAV()->GPUHandle);
            commands->SetPipelineState(advance);
            commands->Dispatch((count + 127) / 128, 1, 1);
            queue->ExecuteCommandList(commands); queue->Flush();
            const auto actual = fixture.Read(display);
            for (UINT i = 0; i < count; ++i)
            {
                const Particle expected{incoming[i], initial[i].Velocity, hasNew ? initial[i].Position : incoming[i]};
                Require(std::memcmp(&actual[i], &expected, sizeof(Particle)) == 0,
                    "Local AdvanceDisplay lost positions, displayed history or preserved velocity");
            }
        }
        DebugErrors(fixture.Device.GetDevice().Get());
        std::cout << "PASS local transfer kernels only; cross-adapter not checked (257 particles, exact Pack, new/repeated display)\n";
    }

    GDX12FluidInstance Seed(bool reference, UINT count)
    {
        GDX12FluidInstance input;
        input.Id = 17;
        input.DeltaSeconds = .0002f;
        input.ContainerToWorld = Matrix::CreateScale(1.1f) * Matrix::CreateTranslation(.2f, -.1f, .3f);
        GDX12FluidSettings settings;
        settings.ReferenceModel = reference;
        settings.MaxSubsteps = 3;
        settings.SmoothingRadiusCm = .2f;
        settings.ParticleRadiusCm = .01f;
        settings.ParticleMassKg = .001f;
        settings.RestDensityKgPerCm3 = 1;
        settings.PressureStiffnessCm2PerSecond2 = 10;
        settings.NearPressureStiffnessCm2PerSecond2 = reference ? 2.15f : 0;
        settings.PredictionSeconds = 1.f / 120;
        settings.AccelerationCmPerSecond2 = Vector3(0, -1, 0);
        settings.ViscosityCm2PerSecond = .01f;
        settings.WaveAcceleration = .25f;
        settings.WavePeriodSeconds = 3;
        input.Component.SetSettings(settings);
        std::vector<GDX12FluidParticle> particles(count);
        for (UINT i = 0; i < count; ++i)
        {
            particles[i].Position = Vector3((static_cast<int>(i % 17) - 8) * .11f,
                (static_cast<int>((i / 17) % 17) - 8) * .11f,
                (static_cast<int>(i / (17 * 17)) - 2) * .11f);
            particles[i].Velocity = Vector3(static_cast<float>(i % 3) * .01f, -.01f,
                static_cast<float>(i % 7) * -.01f);
        }
        input.Component.SetParticles(std::move(particles));
        return input;
    }

    float Compare(const XMFLOAT3& expected, const XMFLOAT3& actual)
    {
        const float a[] = {expected.x, expected.y, expected.z};
        const float b[] = {actual.x, actual.y, actual.z};
        float maximum = 0;
        for (UINT i = 0; i < 3; ++i)
        {
            Require(std::isfinite(a[i]) && std::isfinite(b[i]), "Nonfinite solver state");
            const float error = std::abs(a[i] - b[i]);
            // Cross-vendor FP contraction/rounding can differ; the short deterministic run limits amplification.
            Require(error <= 1e-4f + 1e-4f * (std::max)(std::abs(a[i]), std::abs(b[i])),
                "Equal-step solver states exceed abs=1e-4 + rel=1e-4 tolerance");
            maximum = (std::max)(maximum, error);
        }
        return maximum;
    }

    void ValidateFullRing(Fixture& fixture, ComPtr<IDXGIAdapter4> secondaryAdapter, bool reference)
    {
        auto input = Seed(reference, 257);
        auto buffers = GDX12FluidSolver::CreateBuffers(&fixture.Device, fixture.Heap.get(), input);
        GDX12FluidSolver::Head head;
        GDX12UploadBuffer<GDX12FluidParameters> constants(&fixture.Device, 1);
        GDX12WaterOffload offload(&fixture.Device, fixture.Heap.get(), secondaryAdapter, input);
        uint64_t totalSteps = 0, sequence = 0;
        for (UINT cycle = 0; cycle < 3; ++cycle)
        {
            std::array<std::vector<XMFLOAT3>, 3> sent;
            std::array<uint64_t, 3> publishedSteps;
            std::array<GDX12FluidInstance, 3> publishedInputs;
            for (UINT batch = 0; batch < 4; ++batch)
            {
                input.DeltaSeconds = .0002f + (cycle * 4 + batch) * .00001f;
                input.ClockScale = .5f + batch * .25f;
                input.ContainerToWorld = Matrix::CreateRotationZ((cycle * 4 + batch) * .001f);
                const GDX12FluidSolver::Steps steps{1u + batch % 3, .00005f + batch * .000001f};
                totalSteps += steps.Count;
                fixture.Advance(buffers, head, constants, input, steps);
                offload.Simulate(input, steps);
                if (batch < 3)
                {
                    sent[batch] = offload.ReadSentPositions();
                    publishedSteps[batch] = totalSteps;
                    publishedInputs[batch] = input;
                }
            }
            // The fourth batch must calculate even while all three exported results await display.
            const auto expected = fixture.Read(*buffers.Particles[head.Current]);
            const auto secondary = offload.ReadSolverState();
            Require(expected.size() == secondary.size(), "Full ring changed solver particle count");
            for (size_t i = 0; i < expected.size(); ++i)
            {
                Compare(expected[i].Position, secondary[i].Position);
                Compare(expected[i].Velocity, secondary[i].Velocity);
            }
            for (UINT slot = 0; slot < 3; ++slot)
            {
                const auto previous = fixture.Read(offload.DisplayParticles());
                Require(offload.UpdateDisplay(), "Full ring lost a completed publication");
                const auto result = offload.LastResult();
                Require(result.Sequence == ++sequence && result.Steps == publishedSteps[slot],
                    "Ring FIFO or skipped-export actual-step accounting is wrong");
                Require(result.Input.ClockScale == publishedInputs[slot].ClockScale &&
                    result.Input.DeltaSeconds == publishedInputs[slot].DeltaSeconds &&
                    std::memcmp(&result.Input.ContainerToWorld, &publishedInputs[slot].ContainerToWorld,
                        sizeof(Matrix)) == 0, "Ring publication input snapshot was overwritten");
                const auto received = fixture.Read(offload.DisplayParticles());
                Require(received.size() == sent[slot].size(), "Ring payload particle count changed");
                for (size_t i = 0; i < received.size(); ++i)
                {
                    Require(std::memcmp(&received[i].Position, &sent[slot][i], sizeof(XMFLOAT3)) == 0,
                        "Ring publication payload was overwritten before consumption");
                    Require(std::memcmp(&received[i].PreviousPosition, &previous[i].Position,
                        sizeof(XMFLOAT3)) == 0, "Ring import lost previous displayed positions");
                }
            }
            Require(!offload.UpdateDisplay(), "Unexported full-ring calculation appeared as a publication");
        }
        // Publish once more to expose the last full-ring batch in the cumulative actual-step identity.
        const GDX12FluidSolver::Steps finalBatch{1, .00005f};
        fixture.Advance(buffers, head, constants, input, finalBatch);
        offload.Simulate(input, finalBatch);
        const auto sent = offload.ReadSentPositions();
        Require(offload.UpdateDisplay(), "Freed exchange slot was not reusable");
        const auto result = offload.LastResult();
        Require(result.Sequence == ++sequence && result.Steps == ++totalSteps,
            "Post-full-ring publication omitted calculated but unexported steps");
        const auto received = fixture.Read(offload.DisplayParticles());
        const auto expected = fixture.Read(*buffers.Particles[head.Current]);
        const auto secondary = offload.ReadSolverState();
        for (size_t i = 0; i < received.size(); ++i)
        {
            Require(std::memcmp(&received[i].Position, &sent[i], sizeof(XMFLOAT3)) == 0,
                "Reused exchange slot payload changed in transport");
            Compare(expected[i].Position, secondary[i].Position);
            Compare(expected[i].Velocity, secondary[i].Velocity);
        }
        std::cout << "PASS full three-slot ring reference=" << reference << " cycles=3 publications="
            << sequence << " actual_steps=" << totalSteps << " unexported_batches=3\n";
    }

    void Validate(Fixture& fixture, ComPtr<IDXGIAdapter4> secondaryAdapter)
    {
        ComPtr<ID3D12Device> secondaryDiagnostics;
        ThrowIfFailed(D3D12CreateDevice(secondaryAdapter.Get(), D3D_FEATURE_LEVEL_11_0,
            IID_PPV_ARGS(&secondaryDiagnostics)));
        std::cout << "Numerical tolerance: abs=1e-4 + rel=1e-4 per position/velocity component\n";
        for (bool reference : {false, true})
        {
            for (UINT recreation = 0; recreation < 2; ++recreation)
            {
                const UINT count = recreation ? 1025u : 257u;
                auto input = Seed(reference, count);
                auto buffers = GDX12FluidSolver::CreateBuffers(&fixture.Device, fixture.Heap.get(), input);
                GDX12FluidSolver::Head head;
                GDX12UploadBuffer<GDX12FluidParameters> constants(&fixture.Device, 1);
                fixture.Advance(buffers, head, constants, input, {});
                const auto initial = fixture.Read(*buffers.Particles[head.Current]);
                float maxPositionError = 0, maxVelocityError = 0;
                uint64_t totalSteps = 0;
                {
                    GDX12WaterOffload offload(&fixture.Device, fixture.Heap.get(), secondaryAdapter, input);
                    Require(offload.GetPayloadBytes() == uint64_t(count) * sizeof(XMFLOAT3),
                        "Transport includes data beyond displayed positions");
                    Require(!offload.UpdateDisplay(), "Initial display incorrectly reports a secondary publication");
                    const auto seeded = fixture.Read(offload.DisplayParticles());
                    Require(seeded.size() == initial.size(), "Initial display particle count changed");
                    for (UINT i = 0; i < count; ++i)
                        Require(std::memcmp(&seeded[i].Position, &initial[i].Position, sizeof(XMFLOAT3)) == 0,
                            "Initial water unavailable before first secondary result");

                    for (UINT publication = 1; publication <= 10; ++publication)
                    {
                        const auto previousDisplay = fixture.Read(offload.DisplayParticles());
                        input.ContainerToWorld = Matrix::CreateScale(1.1f) *
                            Matrix::CreateRotationZ(publication * .001f) *
                            Matrix::CreateTranslation(.2f + publication * .002f, -.1f, .3f);
                        auto settings = input.Component.GetSettings();
                        settings.AccelerationCmPerSecond2.x = publication * .01f;
                        settings.WaveAcceleration = .25f + publication * .01f;
                        input.Component.SetSettings(settings);
                        input.DeltaSeconds = .0002f + publication * .00001f;
                        input.ClockScale = .5f + publication * .1f;
                        const GDX12FluidSolver::Steps batch{1u + publication % 3, .00005f + publication * .000001f};
                        totalSteps += batch.Count;
                        fixture.Advance(buffers, head, constants, input, batch);
                        offload.Simulate(input, batch);
                        const auto secondary = offload.ReadSolverState();
                        const auto expected = fixture.Read(*buffers.Particles[head.Current]);
                        Require(secondary.size() == expected.size(), "Secondary solver particle count changed");
                        for (UINT i = 0; i < count; ++i)
                        {
                            maxPositionError = (std::max)(maxPositionError, Compare(expected[i].Position, secondary[i].Position));
                            maxVelocityError = (std::max)(maxVelocityError, Compare(expected[i].Velocity, secondary[i].Velocity));
                        }
                        const auto sent = offload.ReadSentPositions();
                        Require(sent.size() == count, "Sent payload particle count changed");
                        Require(offload.UpdateDisplay(), "Completed secondary result was not imported");
                        const auto result = offload.LastResult();
                        Require(result.Sequence == publication && result.Steps == totalSteps,
                            "Publication or actually executed step identity changed");
                        Require(result.Batch.Count == batch.Count && result.Batch.Dt == batch.Dt &&
                            result.Input.DeltaSeconds == input.DeltaSeconds &&
                            result.Input.ClockScale == input.ClockScale &&
                            result.Input.Component.GetSettings().WaveAcceleration == settings.WaveAcceleration &&
                            std::memcmp(&result.Input.ContainerToWorld, &input.ContainerToWorld, sizeof(Matrix)) == 0,
                            "Publication lost its exact input/dt snapshot");
                        const auto received = fixture.Read(offload.DisplayParticles());
                        for (UINT i = 0; i < count; ++i)
                        {
                            Require(std::memcmp(&received[i].Position, &sent[i], sizeof(XMFLOAT3)) == 0,
                                "Transport payload is not byte-exact");
                            Require(std::memcmp(&received[i].PreviousPosition, &previousDisplay[i].Position,
                                sizeof(XMFLOAT3)) == 0, "Motion references solver history instead of displayed history");
                        }
                        for (UINT repeat = 0; repeat < 3; ++repeat)
                        {
                            Require(!offload.UpdateDisplay(), "Repeated frame imported an old publication again");
                            const auto repeated = fixture.Read(offload.DisplayParticles());
                            for (UINT i = 0; i < count; ++i)
                            {
                                Require(std::memcmp(&repeated[i].Position, &sent[i], sizeof(XMFLOAT3)) == 0,
                                    "Water changed while no new result was ready");
                                Require(std::memcmp(&repeated[i].PreviousPosition, &repeated[i].Position,
                                    sizeof(XMFLOAT3)) == 0, "Repeated result retained stale particle motion");
                            }
                        }
                        DebugErrors(fixture.Device.GetDevice().Get());
                        DebugErrors(secondaryDiagnostics.Get());
                    }
                    // Leave a completed result unconsumed to exercise draining and shared-buffer lifetime.
                    offload.Simulate(input, {1, .00005f});
                }
                fixture.Device.GetCommandQueue()->Flush();
                DebugErrors(fixture.Device.GetDevice().Get());
                DebugErrors(secondaryDiagnostics.Get());
                std::cout << "PASS reference=" << reference << " recreate=" << recreation << " count=" << count
                    << " publications=10 rendered_repeats=30 compared_steps=" << totalSteps
                    << " payload_bytes=" << uint64_t(count) * sizeof(XMFLOAT3)
                    << " max_position_error=" << maxPositionError << " max_velocity_error=" << maxVelocityError << '\n';
            }
            ValidateFullRing(fixture, secondaryAdapter, reference);
            DebugErrors(fixture.Device.GetDevice().Get());
            DebugErrors(secondaryDiagnostics.Get());
        }
        {
            auto input = Seed(true, 257);
            GDX12WaterOffload offload(&fixture.Device, fixture.Heap.get(), secondaryAdapter, input);
            offload.Start();
            offload.Update(input);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            bool imported = false;
            while (!(imported = offload.UpdateDisplay()) && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            offload.Stop();
            Require(imported, "Worker failed to publish without scene rendering or Present before the deadline");
            const auto result = offload.LastResult();
            Require(result.Sequence > 0 && result.Steps > 0, "Worker result contains no executed solver steps");
            const auto particles = offload.ReadSolverState();
            Require(particles.size() == 257, "Worker changed the particle count");
            for (const auto& particle : particles)
            {
                Compare(particle.Position, particle.Position);
                Compare(particle.Velocity, particle.Velocity);
            }
            std::cout << "PASS independent worker without Present: publication=" << result.Sequence
                << " steps=" << result.Steps << '\n';
        }
        DebugErrors(fixture.Device.GetDevice().Get());
        DebugErrors(secondaryDiagnostics.Get());
    }
}

int main(int argc, char** argv)
{
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    try
    {
        bool list = false, validate = false, local = false;
        UINT primary = UINT_MAX, secondary = UINT_MAX;
        for (int i = 1; i < argc; ++i)
        {
            const std::string argument = argv[i];
            if (argument == "--list") list = true;
            else if (argument == "--validate") validate = true;
            else if (argument == "--local") local = true;
            else if (argument == "--primary" && i + 1 < argc) primary = static_cast<UINT>(std::stoul(argv[++i]));
            else if (argument == "--secondary" && i + 1 < argc) secondary = static_cast<UINT>(std::stoul(argv[++i]));
            else throw std::invalid_argument("Usage: --list | --local --primary N | --validate --primary N --secondary N");
        }
        Require(unsigned(list) + unsigned(validate) + unsigned(local) == 1, "Choose exactly one of --list, --local and --validate");
        if (validate || local)
        {
            ComPtr<ID3D12Debug> debug;
            ThrowIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
            debug->EnableDebugLayer();
            ComPtr<ID3D12Debug1> validation;
            ThrowIfFailed(debug.As(&validation));
            validation->SetEnableGPUBasedValidation(TRUE);
        }
        const auto adapters = GDX12DeviceFactory::GetDeviceDescriptors();
        for (size_t i = 0; i < adapters.size(); ++i)
            std::wcout << L"Adapter " << i << L": " << adapters[i].Name << L" LUID="
                << adapters[i].AdapterLuid.HighPart << L":" << adapters[i].AdapterLuid.LowPart << '\n';
        if (list) return 0;
        if (local)
        {
            Require(primary < adapters.size(), "Specify an available primary hardware adapter");
            Fixture fixture(adapters[primary].Adapter);
            ValidateLocalKernels(fixture);
            return 0;
        }
        Require(primary < adapters.size() && secondary < adapters.size() && primary != secondary,
            "UNSUPPORTED: specify two distinct available hardware adapters; no GPU validation executed");
        Fixture fixture(adapters[primary].Adapter);
        Validate(fixture, adapters[secondary].Adapter);
        std::cout << "All equal-step, byte transport, repeat-display and recreation checks passed\n";
        return 0;
    }
    catch (const DxException& error) { std::wcerr << error.ToString() << '\n'; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
    return 1;
}
