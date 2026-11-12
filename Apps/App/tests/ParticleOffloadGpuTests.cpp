#include "../../../Engine/Engine.RendererDX12/src/GDX12ParticleOffload.h"
#include "App.Base/ECS/Components/ParticleEmitterComponent.h"
#include "Common/ConsoleVariables.h"
#include <d3d12sdklayers.h>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <map>

using namespace ParticleResources;
namespace
{
    void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
    void DebugErrors(ID3D12Device* device)
    {
        ComPtr<ID3D12InfoQueue> info;
        ThrowIfFailed(device->QueryInterface(IID_PPV_ARGS(&info)));
        for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i)
        {
            SIZE_T bytes = 0;
            ThrowIfFailed(info->GetMessage(i, nullptr, &bytes));
            std::vector<BYTE> data(bytes);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(data.data());
            ThrowIfFailed(info->GetMessage(i, message, &bytes));
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) throw std::runtime_error(message->pDescription);
        }
        info->ClearStoredMessages();
    }
    template<class T> std::vector<T> Read(GDX12Device& device, GDX12Resource& source, UINT count)
    {
        const auto bytes = uint64_t(count) * sizeof(T);
        auto readback = device.CreateResource(D3D12_HEAP_TYPE_READBACK,
            CD3DX12_RESOURCE_DESC::Buffer(bytes), D3D12_RESOURCE_STATE_COPY_DEST);
        auto* queue = device.GetCommandQueue();
        auto* commands = queue->GetCommandList();
        const auto prior = source.GetCurrentState();
        commands->ResourceBarrier({source.GetCopySourceBarrier()});
        commands->GetCommandList()->CopyBufferRegion(readback.Get(), 0, source.D3DResource.Get(), 0, bytes);
        commands->ResourceBarrier({source.GetTransitionBarrier(prior)});
        queue->ExecuteCommandList(commands); queue->CPUWaitForFenceValue(commands->FenceValue);
        void* mapped = nullptr; ThrowIfFailed(readback->Map(0, nullptr, &mapped));
        std::vector<T> result(count); std::memcpy(result.data(), mapped, size_t(bytes));
        const D3D12_RANGE written{0, 0}; readback->Unmap(0, &written);
        return result;
    }
    struct Fixture
    {
        GDX12Device Device;
        GDX12DeviceResources Resources;
        RenderPipelineCommonData Common;
        GDX12Texture Depth;
        explicit Fixture(ComPtr<IDXGIAdapter4> adapter)
        {
            ThrowIfFailed(Device.Initialize(adapter));
            Resources.Device = &Device;
            Resources.SRV_UAV_Heap = std::make_shared<GDX12DescriptorHeap>(&Device,
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 131072, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
            Resources.FrameConstants.push_back(std::make_unique<GDX12FrameConstants>(&Device));
            Resources.FrameConstants[0]->CameraCB->Resize(1);
            GDX12RenderHelpers::InitializeComputeTexture(Depth, Resources, 2, 2, DXGI_FORMAT_R32_FLOAT);
            Common.ResetHistory = false;
            Common.ActiveCameraCBufferIndex = 0;
        }
        ~Fixture() { Device.GetCommandQueue()->Flush(); }
        template<class T> void Upload(Buffer<T>& target, const std::vector<T>& values)
        {
            Buffer<T> upload(&Device, UINT(values.size()), EBufferType::Upload, false);
            upload.CopyData(0, values.data(), UINT(values.size()));
            auto* queue = Device.GetCommandQueue(); auto* commands = queue->GetCommandList();
            commands->ResourceBarrier({target.GetResource().GetCopyDestBarrier()});
            commands->GetCommandList()->CopyBufferRegion(target.GetResource().D3DResource.Get(), 0,
                upload.GetResource().D3DResource.Get(), 0, uint64_t(values.size()) * sizeof(T));
            queue->ExecuteCommandList(commands); queue->CPUWaitForFenceValue(commands->FenceValue);
        }
    };
    void Selection()
    {
        ParticleEmitterComponent component;
        auto settings = component.GetSettings();
        Require(!settings.SharedSimulation && !CanOffloadParticleEmitter(settings), "Default emitter was offloaded");
        settings.SharedSimulation = true; settings.SoftDistance = 100;
        settings.CollisionBoxEnabled = settings.VolumeEnabled = settings.InteractionEnabled = true;
        Require(CanOffloadParticleEmitter(settings), "Non-depth simulation modes blocked offload");
        component.SetSettings(settings);
        ParticleEmitterComponent copy(component), assigned; assigned = component;
        Require(copy.GetSettings().SharedSimulation && assigned.GetSettings().SharedSimulation &&
            copy.GetId() != component.GetId(), "Component copies lost the explicit opt-in or identity semantics");
        settings.DepthCollision = true;
        Require(!CanOffloadParticleEmitter(settings), "Depth collision was offloaded");
        settings.DepthCollision = false; Require(CanOffloadParticleEmitter(settings), "Depth toggle did not restore eligibility");
        settings.SharedSimulation = false; Require(!CanOffloadParticleEmitter(settings), "Unmarked emitter was offloaded");
        std::cout << "PASS CPU selection, depth toggle, box/volume/interaction/soft and component copies\n";
    }
    void Kernels(Fixture& fixture)
    {
        constexpr UINT count = 257, words = 16 + count * 9;
        auto source = CreateBuffer<Particle>(&fixture.Resources, count);
        auto counters = CreateBuffer<UINT>(&fixture.Resources, 8);
        auto packed = CreateBuffer<UINT>(&fixture.Resources, words);
        auto display = CreateBuffer<Particle>(&fixture.Resources, count);
        std::vector<Particle> seed(count);
        for (UINT i = 0; i < count; ++i)
        {
            seed[i].Position = {float(i), 2, -3}; seed[i].Age = .25f; seed[i].Lifetime = 2;
            seed[i].Rotation = .125f; seed[i].SizeRandom = .75f; seed[i].Identity = {i + 1, 7};
        }
        fixture.Upload(*counters, std::vector<UINT>{0, count, 0, 0, 0, 0, 0, 0});
        GDX12RootSignatureDesc desc; desc.VolatileSRVData = true;
        desc.Constants = {1, 1, 1, 16}; desc.NumSingleSRVSlots = 3; desc.NumSingleUAVSlots = 2;
        GDX12RootSignature root(&fixture.Device, desc);
        const auto pack = GDX12RenderHelpers::CreateComputePipeline(&fixture.Device, &root, SHADERS_FOLDER "ParticleTransfer.hlsl", "Pack");
        const auto advance = GDX12RenderHelpers::CreateComputePipeline(&fixture.Device, &root, SHADERS_FOLDER "ParticleTransfer.hlsl", "AdvanceDisplay");
        std::array<UINT, 16> params{count, 16, 8, 0, 42, 1, 3, 2, 5, 4, 1, 0, words, 1, 1, 1};
        const auto execute = [&](bool repack)
        {
            auto* queue = fixture.Device.GetCommandQueue(); auto* commands = queue->GetCommandList();
            commands->SetDescriptorHeaps({fixture.Resources.SRV_UAV_Heap.get()}); commands->SetComputeRootSignature(&root);
            commands->GetCommandList()->SetComputeRoot32BitConstants(3, 16, params.data(), 0);
            if (repack)
            {
                commands->ResourceBarrier({source->GetResource().GetNonPixelShaderResourceBarrier(), counters->GetResource().GetNonPixelShaderResourceBarrier()});
                UAV(commands, *packed); commands->SetComputeSRV(0, source->GetSRV()->GPUHandle);
                commands->SetComputeSRV(1, counters->GetSRV()->GPUHandle); commands->SetComputeUAV(0, packed->GetUAV()->GPUHandle);
                commands->SetPipelineState(pack); commands->Dispatch((count + 255) / 256, 1, 1);
            }
            commands->ResourceBarrier({packed->GetResource().GetNonPixelShaderResourceBarrier()}); UAV(commands, *display);
            commands->SetComputeSRV(2, packed->GetSRV()->GPUHandle); commands->SetComputeUAV(1, display->GetUAV()->GPUHandle);
            commands->SetPipelineState(advance); commands->Dispatch((count + 255) / 256, 1, 1);
            queue->ExecuteCommandList(commands); queue->CPUWaitForFenceValue(commands->FenceValue);
            return Read<Particle>(fixture.Device, display->GetResource(), count);
        };
        fixture.Upload(*source, seed);
        auto shown = execute(true);
        const auto payload = Read<UINT>(fixture.Device, packed->GetResource(), words);
        Require(payload[0] == 1 && payload[2] == 5 && payload[3] == 4 && payload[4] == 1 && payload[5] == words &&
            payload[8] == 42 && payload[9] == 1 && payload[10] == 3 && payload[11] == 2 &&
            payload[12] == 16 && payload[13] == count && payload[14] == count, "Packed metadata disagrees with the independent oracle");
        for (UINT i = 0; i < count; ++i)
        {
            Require(shown[i].Position.x == float(i) && shown[i].Previous.x == float(i) &&
                shown[i].Age == .25f && shown[i].Lifetime == 2 && shown[i].Rotation == .125f &&
                shown[i].SizeRandom == .75f && shown[i].Identity.y == 7, "Initial compact particle payload is wrong");
        }
        for (auto& p : seed) { p.Position.x += 10; p.Age += .1f; }
        params[15] = 0; fixture.Upload(*source, seed); shown = execute(true);
        Require(shown[0].Previous.x == 0 && shown[0].Position.x == 10 && shown[0].PreviousAge == .25f, "Displayed history used the solver step");
        params[14] = 0; shown = execute(false);
        Require(shown[0].Previous.x == shown[0].Position.x && shown[0].PreviousAge == shown[0].Age, "Repeated display retained simulation motion");
        params[15] = 2; shown = execute(false);
        Require(shown[0].Lifetime == 2 && shown[0].Position.x == 10, "Camera reset erased the displayed state");
        seed[0].Identity.x += 1000; seed[0].Position.x = 50; seed[1].Lifetime = 0;
        params[14] = 1; params[15] = 0; fixture.Upload(*source, seed); shown = execute(true);
        Require(shown[0].Previous.x == 50 && shown[1].Lifetime == 0, "Birth identity or empty/death publication is wrong");
        params[8] += 1; shown = execute(false);
        Require(shown[0].Lifetime == 0, "Stale layout generation was accepted");
        DebugErrors(fixture.Device.GetDevice().Get());
        std::cout << "PASS GPU compact payload, metadata, repeat/new-birth/death motion and generation rejection\n";
    }
    void Compare(float a, float b)
    {
        Require(std::isfinite(a) && std::isfinite(b) && std::abs(a - b) <= 1e-4f + 1e-4f * (std::max)(std::abs(a), std::abs(b)),
            "Equal-step cross-adapter particle mismatch");
    }
    void CrossAdapter(Fixture& fixture, ComPtr<IDXGIAdapter4> secondary)
    {
        for (const auto mode : {ParticleSimulation::AnalyticDrag, ParticleSimulation::SemiImplicitEuler})
        {
            GDX12ParticleOffload offload(&fixture.Resources, secondary);
            GDX12ParticleSimulation reference;
            std::vector<GDX12ParticleEmitter> emitters;
            for (UINT i = 0; i < 3; ++i)
            {
                GDX12ParticleEmitter emitter; emitter.Id = 41 + i; emitter.MaxParticles = i == 0 ? 17 : i == 1 ? 257 : 33;
                emitter.BurstTotal = i == 2 ? 0 : emitter.MaxParticles / 2; emitter.EmissionRate = 0;
                emitter.LifetimeMin = emitter.LifetimeMax = .045f; emitter.Substeps = 2; emitter.DeltaTime = .01f;
                emitter.Simulation = mode; emitter.SharedSimulation = true; emitter.Seed = 37 + i;
                emitter.CollisionBoxEnabled = i != 0; emitter.CollisionBoxCenter = Vector3(0, .5f, 0);
                emitter.CollisionBoxHalfExtents = Vector3(10, .25f, 10); emitter.CollisionRadius = .1f;
                emitter.VolumeEnabled = i != 1; emitter.VolumeHalfExtents = Vector3(.25f); emitter.InteractionEnabled = true;
                emitters.push_back(emitter);
            }
            auto field = CreateBuffer<XMFLOAT4>(&fixture.Resources, 16);
            fixture.Upload(*field, std::vector<XMFLOAT4>(16, XMFLOAT4(.25f, .1f, .75f, 30)));
            const GDX12ParticleInteractionField interaction{&field->GetResource(), field->GetSRV()->GPUHandle, {0, 0, 100, 100}, 4, -10};
            reference.SetInteractionField(interaction);
            for (UINT step = 0; step < 12; ++step)
            {
                if (step == 2) { emitters[0].Paused = true; emitters[0].BurstTotal += 3; }
                if (step == 3) emitters[0].Paused = false;
                if (step == 5) { ++emitters[1].ResetRevision; emitters[1].BurstTotal = 7; }
                if (step == 7) emitters[2].BurstTotal = 5;
                if (step == 9) { emitters[0].TimeScale = .25f; emitters[1].World = Matrix::CreateTranslation(1, 0, 2); }
                offload.Configure(emitters, 4);
                const auto before = offload.GetStatistics();
                auto* queue = fixture.Device.GetCommandQueue(); auto* commands = queue->GetCommandList();
                offload.Record(commands, emitters, interaction, false); queue->ExecuteCommandList(commands);
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
                while (offload.GetStatistics().Steps == before.Steps && std::chrono::steady_clock::now() < deadline)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                Require(offload.GetStatistics().Steps == before.Steps + 1, "Particle worker failed to execute one real step");
                offload.Stop();
                const auto sent = offload.ReadPayload(true);
                commands = queue->GetCommandList(); offload.Record(commands, emitters, interaction, false);
                queue->ExecuteCommandList(commands); queue->CPUWaitForFenceValue(commands->FenceValue);
                Require(sent == offload.ReadPayload(false), "Common payload is not bit-identical across adapters");
                const auto statistics = offload.GetStatistics();
                Require(statistics.ExportCopies == statistics.Publications && statistics.ImportCopies == statistics.Imports &&
                    statistics.Imports == step + 1, "Transport copy count grew with emitter count or skipped a publication");
                fixture.Resources.ParticleEmitters = emitters;
                for (auto& emitter : fixture.Resources.ParticleEmitters) emitter.SharedSimulation = false;
                reference.BeginFrame(&fixture.Resources, fixture.Common);
                commands = queue->GetCommandList(); reference.InitializePools(commands, &fixture.Resources);
                reference.Execute(commands, &fixture.Resources, fixture.Common, &fixture.Depth);
                queue->ExecuteCommandList(commands); queue->CPUWaitForFenceValue(commands->FenceValue);
                for (const auto& emitter : emitters)
                {
                    const auto actual = offload.ReadSolver(emitter.Id);
                    GDX12Resource* resource = nullptr;
                    for (auto* r : reference.GetPoolResources())
                        if (r->D3DResource->GetDesc().Width == uint64_t(emitter.MaxParticles) * sizeof(Particle)) resource = r;
                    Require(resource, "Missing local reference pool");
                    const auto expected = Read<Particle>(fixture.Device, *resource, emitter.MaxParticles);
                    std::map<uint64_t, Particle> byBirth;
                    for (const auto& p : expected) if (p.Lifetime > p.Age)
                        byBirth.emplace(uint64_t(p.Identity.y) << 32 | p.Identity.x, p);
                    UINT alive = 0;
                    for (const auto& p : actual) if (p.Lifetime > p.Age)
                    {
                        if (emitter.Id == 41)
                            Require(std::abs(p.Position.x) <= .25001f && std::abs(p.Position.y) <= .25001f &&
                                std::abs(p.Position.z) <= .25001f, "Secondary volume confinement did not act on moving particles");
                        if (emitter.Id == 42)
                            Require(p.Position.y <= .15001f, "Secondary box collision allowed a particle through the obstacle");
                        ++alive; const auto found = byBirth.find(uint64_t(p.Identity.y) << 32 | p.Identity.x);
                        Require(found != byBirth.end(), "Cross-adapter birth identity mismatch");
                        const auto& e = found->second;
                        Compare(p.Position.x, e.Position.x); Compare(p.Position.y, e.Position.y); Compare(p.Position.z, e.Position.z);
                        Compare(p.Velocity.x, e.Velocity.x); Compare(p.Velocity.y, e.Velocity.y); Compare(p.Velocity.z, e.Velocity.z);
                        Compare(p.Age, e.Age); Compare(p.Lifetime, e.Lifetime); Compare(p.Rotation, e.Rotation);
                    }
                    Require(alive == byBirth.size(), "Cross-adapter live count mismatch");
                }
                DebugErrors(fixture.Device.GetDevice().Get()); DebugErrors(offload.SecondaryDevice());
            }
            const auto statistics = offload.GetStatistics();
            Require(statistics.Publications == 12 && statistics.ExportCopies == 12 && statistics.ImportCopies == 12,
                "Expected one common export and one import per three-emitter publication");
            for (auto count : statistics.SlotPublications) Require(count == 4, "Three-slot ring did not complete four rotations");
            // Remove/reorder/reallocate, retain solver state of the unaffected emitter and drain queued shutdown.
            const auto retained = offload.ReadSolver(emitters[0].Id);
            emitters.erase(emitters.begin() + 1); emitters[1].MaxParticles = 65;
            offload.Configure(emitters, 4); offload.Stop();
            const auto after = offload.ReadSolver(emitters[0].Id);
            Require(retained.size() == after.size() && std::memcmp(retained.data(), after.data(), retained.size() * sizeof(Particle)) == 0,
                "Range reallocation reset an unaffected solver");
            std::cout << "PASS two-GPU mode=" << UINT(mode) << " emitters=3 actual_steps=12 publications=12 export_copies=12 import_copies=12 bytes="
                << statistics.PayloadBytes << " interaction copies=" << statistics.InputCopies << "\n";
        }
    }

    void Placement(Fixture& fixture, UINT secondary)
    {
        Require(ConsoleModule{}.FindConsoleVariable(L"Render.Particles.SecondaryAdapter").SetFromString(std::to_string(secondary)),
            "Cannot configure startup-only particle adapter");
        GDX12ParticleSimulation simulation;
        fixture.Resources.ParticleEmitters.clear(); simulation.BeginFrame(&fixture.Resources, fixture.Common);
        Require(simulation.GetPoolResources().empty() && !simulation.GetImportedResource(), "Zero-emitter state allocated simulation");
        GDX12ParticleEmitter a; a.Id = 101; a.MaxParticles = 7; a.BurstTotal = 3;
        a.EmissionRate = 0; a.SharedSimulation = true; a.SoftDistance = 20;
        auto b = a; b.Id = 102; b.MaxParticles = 11; b.VolumeEnabled = b.CollisionBoxEnabled = true;
        auto c = a; c.Id = 103; c.MaxParticles = 13; c.DepthCollision = true;
        const auto frame = [&]
        {
            simulation.BeginFrame(&fixture.Resources, fixture.Common);
            auto* queue = fixture.Device.GetCommandQueue(); auto* commands = queue->GetCommandList();
            simulation.InitializePools(commands, &fixture.Resources);
            simulation.ExecuteOffload(commands);
            simulation.Execute(commands, &fixture.Resources, fixture.Common, &fixture.Depth);
            queue->ExecuteCommandList(commands); queue->CPUWaitForFenceValue(commands->FenceValue);
        };
        fixture.Resources.ParticleEmitters = {a}; frame();
        Require(simulation.GetPoolResources().empty() && simulation.GetImportedResource(), "One marked soft emitter did not offload");
        fixture.Resources.ParticleEmitters = {a, b, c}; frame();
        Require(simulation.GetPoolResources().size() == 6 && simulation.GetImportedResource()->D3DResource->GetDesc().Width == 18 * sizeof(Particle),
            "Not all eligible emitters offloaded or depth emitter lost its local pool");
        a.DepthCollision = true; fixture.Resources.ParticleEmitters = {a, b, c}; frame();
        Require(simulation.GetPoolResources().size() == 12 && simulation.GetImportedResource()->D3DResource->GetDesc().Width == 11 * sizeof(Particle),
            "Depth toggle did not return the emitter to primary before the next step");
        b.SharedSimulation = false; fixture.Resources.ParticleEmitters = {a, b, c}; frame();
        Require(simulation.GetPoolResources().size() == 18 && !simulation.GetImportedResource(), "Opt-out kept stale imported ranges");
        a.DepthCollision = false; b.SharedSimulation = true;
        fixture.Resources.ParticleEmitters = {a, b}; frame();
        ++a.ResetRevision; a.MaxParticles = 19; fixture.Resources.ParticleEmitters = {a}; frame();
        Require(simulation.GetPoolResources().empty() && simulation.GetImportedResource()->D3DResource->GetDesc().Width == 19 * sizeof(Particle),
            "Reset/capacity/removal retained old ranges");
        fixture.Resources.ParticleEmitters.clear(); frame();
        Require(!simulation.GetImportedResource(), "Removed effect survived in an old publication");
        a.DepthCollision = false; a.MaxParticles = 4096; a.BurstTotal = 0;
        b.MaxParticles = 4097; b.BurstTotal = 0; b.SharedSimulation = true;
        const auto enqueue = [&]
        {
            fixture.Resources.ParticleEmitters = {a, b};
            simulation.BeginFrame(&fixture.Resources, fixture.Common);
            auto* queue = fixture.Device.GetCommandQueue(); auto* commands = queue->GetCommandList();
            simulation.InitializePools(commands, &fixture.Resources); simulation.ExecuteOffload(commands);
            queue->ExecuteCommandList(commands);
        };
        for (UINT i = 0; i < 100; ++i)
        {
            ++a.BurstTotal; b.BurstTotal += 2; a.Paused = i >= 20 && i < 60;
            if (i == 50) { ++b.ResetRevision; b.BurstTotal = 2; }
            enqueue(); // Deliberately no CPU wait while jobs are in flight.
        }
        a.Paused = false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        UINT aliveA = 0, aliveB = 0;
        do
        {
            enqueue();
            Require(simulation.GetPoolResources().empty(), "Busy offloaded emitters also simulated locally");
            const auto particles = Read<Particle>(fixture.Device, *simulation.GetImportedResource(), a.MaxParticles + b.MaxParticles);
            aliveA = aliveB = 0;
            for (UINT i = 0; i < particles.size(); ++i)
                if (particles[i].Lifetime > particles[i].Age) ++(i < a.MaxParticles ? aliveA : aliveB);
        } while ((aliveA != a.BurstTotal || aliveB != b.BurstTotal) && std::chrono::steady_clock::now() < deadline);
        Require(aliveA == a.BurstTotal && aliveB == b.BurstTotal, "Busy-worker cadence lost or repeated pending bursts/reset");
        fixture.Resources.ParticleEmitters.clear(); frame();
        DebugErrors(fixture.Device.GetDevice().Get());
        std::cout << "PASS production placement 0/1/many, mixed local/imported, depth toggle, opt-out, reset/capacity/removal; queued burst totals=100/100\n";
    }
}
int main(int argc, char** argv)
{
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    std::cout << std::unitbuf;
    try
    {
        bool list = false, local = false, gpuValidation = true; UINT primary = 0, secondary = UINT_MAX;
        for (int i = 1; i < argc; ++i)
        {
            const std::string arg = argv[i];
            if (arg == "--list") list = true;
            else if (arg == "--local") local = true;
            else if (arg == "--validate") local = false;
            else if (arg == "--no-gpu-validation") gpuValidation = false;
            else if (arg == "--primary" && i + 1 < argc) primary = UINT(std::stoul(argv[++i]));
            else if (arg == "--secondary" && i + 1 < argc) secondary = UINT(std::stoul(argv[++i]));
            else throw std::invalid_argument("Usage: --list | --local --primary N | --validate --primary N --secondary N");
        }
        if (!list)
        {
            ComPtr<ID3D12Debug> debug; ThrowIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))); debug->EnableDebugLayer();
            if (gpuValidation)
            {
                ComPtr<ID3D12Debug1> validation; ThrowIfFailed(debug.As(&validation)); validation->SetEnableGPUBasedValidation(TRUE);
            }
        }
        const auto adapters = GDX12DeviceFactory::GetDeviceDescriptors();
        for (UINT i = 0; i < adapters.size(); ++i) std::cout << "Adapter " << i << ": " << WStringToString(adapters[i].Name) << '\n';
        if (list) return 0;
        Selection(); Require(primary < adapters.size(), "Missing primary adapter");
        Fixture fixture(adapters[primary].Adapter); Kernels(fixture);
        if (!local)
        {
            Require(secondary < adapters.size() && secondary != primary, "UNSUPPORTED: two distinct hardware adapters are required");
            CrossAdapter(fixture, adapters[secondary].Adapter);
            Placement(fixture, secondary);
        }
        DebugErrors(fixture.Device.GetDevice().Get());
        std::cout << "All requested particle checks passed\n"; return 0;
    }
    catch (const DxException& error)
    {
        std::cerr << "DX failure HRESULT=0x" << std::hex << unsigned(error.ErrorCode) << std::dec
            << " line=" << error.LineNumber << " function=" << WStringToString(error.FunctionName) << '\n';
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
    return 1;
}
