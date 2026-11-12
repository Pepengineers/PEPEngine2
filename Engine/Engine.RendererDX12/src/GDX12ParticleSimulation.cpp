#include "GDX12ParticleOffload.h"
#include "Common/ConsoleVariables.h"
#include "Engine.Core/Logging/Logger.h"
#include <algorithm>
#include <stdexcept>
#include <unordered_set>

using namespace ParticleResources;

namespace
{
    AutoConsoleVariable<int> ParticleAdapter(L"Render.Particles.SecondaryAdapter", -1,
        L"Particle simulation adapter; -1 uses the water adapter when selected, otherwise single GPU. Startup only.", EConsoleVariableFlags::ReadOnly);

    ComPtr<IDXGIAdapter4> SecondaryAdapter(GDX12Device* primary)
    {
        int index = ParticleAdapter.GetValue();
        ConsoleModule console;
        IConsoleVariable* water = nullptr;
        if (console.TryFindConsoleVariable(L"Render.Fluid.SecondaryAdapter", water) && water->GetInt() >= 0)
        {
            if (index >= 0 && index != water->GetInt())
                throw std::invalid_argument("Particles and water must select the same secondary adapter");
            index = water->GetInt();
        }
        if (index < 0) return {};
        const auto adapters = GDX12DeviceFactory::GetDeviceDescriptors();
        if (size_t(index) >= adapters.size()) return {};
        const auto luid = primary->GetAdapterLuid();
        const auto other = adapters[index].AdapterLuid;
        if (luid.HighPart == other.HighPart && luid.LowPart == other.LowPart) return {};
        return adapters[index].Adapter;
    }
}

GDX12ParticleSimulation::State::State(GDX12DeviceResources* resources, bool withoutDepth)
{
    Root = CreateRoot(resources, true);
    const D3D_SHADER_MACRO defines[] = {{"PARTICLE_NO_DEPTH", "1"}, {nullptr, nullptr}};
    for (const auto* entry : { "InitializePool", "PrepareSimulation", "Simulate", "PrepareSpawn", "Spawn", "PublishAlive" })
        Compute[entry] = GDX12RenderHelpers::CreateComputePipeline(resources->Device, Root.get(),
            SHADERS_FOLDER "ParticleSimulation.hlsl", entry, withoutDepth ? defines : nullptr);
    D3D12_INDIRECT_ARGUMENT_DESC argument = {};
    argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    D3D12_COMMAND_SIGNATURE_DESC signature = {};
    signature.ByteStride = sizeof(D3D12_DISPATCH_ARGUMENTS);
    signature.NumArgumentDescs = 1;
    signature.pArgumentDescs = &argument;
    ThrowIfFailed(resources->Device->GetDevice()->CreateCommandSignature(&signature, nullptr, IID_PPV_ARGS(&DispatchSignature)));
}

GDX12ParticleSimulation::GDX12ParticleSimulation() = default;
GDX12ParticleSimulation::~GDX12ParticleSimulation() = default;

std::vector<GDX12Resource*> GDX12ParticleSimulation::GetOffloadResources() const
{
    return _state && _state->Active && _state->Offload ? _state->Offload->Resources() : std::vector<GDX12Resource*>{};
}
GDX12Resource* GDX12ParticleSimulation::GetImportedResource() const
{
    const auto resources = GetOffloadResources();
    return resources.empty() ? nullptr : resources.front();
}
GDX12Resource* GDX12ParticleSimulation::GetOffloadInteractionResource() const
{
    if (_state && _state->Active)
        for (const auto& emitter : _state->Emitters)
            if (_state->Offloaded.count(emitter.Id) && emitter.InteractionEnabled) return _interaction.Resource;
    return nullptr;
}
void GDX12ParticleSimulation::ExecuteOffload(GDX12CommandList* commands)
{
    if (_state && _state->Active && _state->Offload)
        _state->Offload->Record(commands, _state->Emitters, _interaction, _state->ResetMotion);
}

std::vector<GDX12Resource*> GDX12ParticleSimulation::GetPoolResources() const
{
    std::vector<GDX12Resource*> result;
    if (_state && _state->Active)
        for (const auto& entry : _state->Pools)
        {
            const auto& pool = *entry.second;
            result.insert(result.end(), { &pool.Particles->GetResource(), &pool.Alive[0]->GetResource(),
                &pool.Alive[1]->GetResource(), &pool.Dead->GetResource(), &pool.Counters->GetResource(), &pool.Args->GetResource() });
        }
    return result;
}

void GDX12ParticleSimulation::BeginFrame(GDX12DeviceResources* resources, RenderPipelineCommonData& common)
{
    resources->ParticleMotion = nullptr;
    const auto global = GetParticleSettings();
    if (resources->ParticleEmitters.empty())
    {
        if (_state)
        {
            resources->Device->GetCommandQueue()->Flush();
            _state.reset();
            common.ResetUpscalerHistory = true;
        }
        return;
    }
    if (!global.Enabled)
    {
        if (_state)
        {
            _state->Active = false;
            if (_state->Offload) _state->Offload->Stop();
        }
        return;
    }
    std::vector<GDX12ParticleEmitter> emitters = resources->ParticleEmitters;
    std::unordered_set<uint64_t> ids;
    UINT total = 0;
    for (auto& emitter : emitters)
    {
        if (!emitter.Id || !ids.insert(emitter.Id).second || !IsValidParticleEmitterSettings(emitter) ||
            !std::isfinite(emitter.DeltaTime) || emitter.DeltaTime < 0)
            throw std::invalid_argument("Invalid particle emitter snapshot");
        const auto* matrix = reinterpret_cast<const float*>(&emitter.World);
        for (UINT i = 0; i < 16; ++i)
            if (!std::isfinite(matrix[i])) throw std::invalid_argument("Nonfinite particle transform");
        const float determinant = emitter.World.Determinant();
        if (!std::isfinite(determinant) || std::abs(determinant) < 0.000001f)
            throw std::invalid_argument("Singular particle transform");
        if (emitter.World._14 != 0 || emitter.World._24 != 0 || emitter.World._34 != 0 || emitter.World._44 != 1)
            throw std::invalid_argument("Particle transform must be affine");
        static_cast<GDX12ParticleEmitterSettings&>(emitter) = ResolveParticleEmitterSettings(emitter, global.Quality);
        if (emitter.MaxParticles > 4194304 - total) throw std::length_error("Particle scene capacity exceeds 4194304");
        total += emitter.MaxParticles;
        emitter.Paused |= !global.SimulationEnabled;
    }
    if (!_state) _state = std::make_unique<State>(resources);
    auto& state = *_state;
    state.Active = true;
    state.ResetMotion = common.ResetHistory;
    std::vector<GDX12ParticleEmitter> remote;
    ComPtr<IDXGIAdapter4> adapter;
    if (std::any_of(emitters.begin(), emitters.end(), CanOffloadParticleEmitter))
    {
        if (!state.AdapterResolved)
        {
            state.Secondary = SecondaryAdapter(resources->Device);
            state.AdapterResolved = true;
        }
        adapter = state.Secondary;
    }
    std::unordered_set<uint64_t> offloaded;
    for (auto it = state.DepthWarnings.begin(); it != state.DepthWarnings.end();)
        if (!ids.count(*it)) it = state.DepthWarnings.erase(it);
        else ++it;
    for (const auto& emitter : emitters)
    {
        if (emitter.SharedSimulation && emitter.DepthCollision)
        {
            if (state.DepthWarnings.insert(emitter.Id).second)
                Log::Info("Particles", "Emitter ", emitter.Id, " stays on primary: DepthCollision is enabled");
        }
        else state.DepthWarnings.erase(emitter.Id);
        if (adapter && CanOffloadParticleEmitter(emitter))
        {
            remote.push_back(emitter);
            offloaded.insert(emitter.Id);
        }
    }
    std::sort(remote.begin(), remote.end(), [](const auto& a, const auto& b) { return a.Id < b.Id; });
    std::unordered_map<uint64_t, uint64_t> incomingBursts, outgoingBursts;
    if (offloaded != state.Offloaded)
    {
        if (state.Offload) state.Offload->Stop();
        for (const auto& emitter : emitters)
        {
            const bool was = state.Offloaded.count(emitter.Id) != 0, now = offloaded.count(emitter.Id) != 0;
            if (was == now) continue;
            uint64_t consumed = 0;
            if (was) consumed = state.Offload->ConsumedBurst(emitter.Id);
            else if (const auto found = state.Pools.find(emitter.Id); found != state.Pools.end() &&
                found->second->ResetRevision == emitter.ResetRevision) consumed = found->second->BurstTotal;
            // A user reset cancels older requests; a placement reset retains already consumed bursts.
            const auto previous = std::find_if(state.Emitters.begin(), state.Emitters.end(),
                [&](const auto& e) { return e.Id == emitter.Id; });
            if (previous != state.Emitters.end() && previous->ResetRevision != emitter.ResetRevision) consumed = 0;
            (now ? outgoingBursts : incomingBursts)[emitter.Id] = consumed;
            Log::Info("Particles", "Emitter ", emitter.Id, " placement reset -> ", now ? "secondary" : "primary",
                "; consumed burst=", consumed);
        }
        state.ResetMotion = true;
    }
    if (!remote.empty() && !state.Offload) state.Offload = std::make_unique<GDX12ParticleOffload>(resources, adapter);
    if (state.Offload)
    {
        const bool interaction = std::any_of(remote.begin(), remote.end(), [](const auto& e) { return e.InteractionEnabled; });
        state.Offload->Configure(remote, interaction && _interaction.Resource ? _interaction.Resolution : 0, outgoingBursts);
    }
    bool changed = total != state.Capacity;
    changed |= offloaded != state.Offloaded;
    for (const auto& pool : state.Pools) changed |= ids.count(pool.first) == 0 || offloaded.count(pool.first) != 0;
    for (const auto& emitter : emitters)
    {
        if (offloaded.count(emitter.Id)) continue;
        const auto found = state.Pools.find(emitter.Id);
        changed |= found == state.Pools.end() || found->second->Capacity != emitter.MaxParticles;
    }
    if (changed)
    {
        // All replacements happen before recording any particle work, after earlier submissions retire.
        resources->Device->GetCommandQueue()->Flush();
        for (auto it = state.Pools.begin(); it != state.Pools.end();)
            if (!ids.count(it->first) || offloaded.count(it->first)) it = state.Pools.erase(it);
            else ++it;
        for (const auto& emitter : emitters)
        {
            if (offloaded.count(emitter.Id)) continue;
            auto& pool = state.Pools[emitter.Id];
            if (!pool || pool->Capacity != emitter.MaxParticles)
            {
                const auto consumed = pool && pool->ResetRevision == emitter.ResetRevision ? pool->BurstTotal : 0;
                pool = std::make_unique<Pool>(resources, emitter.MaxParticles);
                pool->ResetRevision = emitter.ResetRevision;
                pool->BurstTotal = incomingBursts.count(emitter.Id) ? incomingBursts.at(emitter.Id) : consumed;
            }
        }
        common.ResetUpscalerHistory = true;
    }
    state.Capacity = total;
    state.Offloaded = std::move(offloaded);
    for (const auto& emitter : emitters)
    {
        if (state.Offloaded.count(emitter.Id)) continue;
        const auto& pool = *state.Pools.at(emitter.Id);
        state.ResetMotion |= !pool.Initialized || pool.ResetRevision != emitter.ResetRevision;
    }
    if (state.ResetMotion) common.ResetUpscalerHistory = true;
    state.Emitters = std::move(emitters);
}

void GDX12ParticleSimulation::InitializePools(GDX12CommandList* commands, GDX12DeviceResources* resources)
{
    if (!_state || !_state->Active) return;
    auto& state = *_state;
    GDX12_SCOPED_EVENT(commands, "Particle pool initialization", Colors::Orange);
    commands->SetComputeRootSignature(state.Root.get());
    commands->SetDescriptorHeaps({resources->SRV_UAV_Heap.get()});
    for (const auto& emitter : state.Emitters)
    {
        if (state.Offloaded.count(emitter.Id)) continue;
        auto& pool = *state.Pools.at(emitter.Id);
        if (pool.Initialized && pool.ResetRevision == emitter.ResetRevision) continue;
        pool.ReadIndex = 0;
        if (pool.ResetRevision != emitter.ResetRevision) pool.BurstTotal = 0;
        pool.ResetRevision = emitter.ResetRevision;
        pool.Initialized = true;
        const UINT parameters[] = {pool.Capacity, 0, state.Capacity, 1};
        commands->GetCommandList()->SetComputeRoot32BitConstants(0, 4, parameters, 0);
        UAV(commands, *pool.Particles);
        UAV(commands, *pool.Dead);
        UAV(commands, *pool.Counters);
        UAV(commands, *pool.Args);
        commands->SetComputeUAV(0, pool.Particles->GetUAV()->GPUHandle);
        commands->SetComputeUAV(2, pool.Dead->GetUAV()->GPUHandle);
        commands->SetComputeUAV(3, pool.Counters->GetUAV()->GPUHandle);
        commands->SetComputeUAV(4, pool.Args->GetUAV()->GPUHandle);
        commands->SetPipelineState(state.Compute.at("InitializePool"));
        commands->Dispatch((pool.Capacity + 255) / 256, 1, 1);
        UAV(commands, *pool.Counters);
        UAV(commands, *pool.Particles);
        UAV(commands, *pool.Dead);
        commands->SetPipelineState(state.Compute.at("PublishAlive"));
        commands->Dispatch(1, 1, 1);
        UAV(commands, *pool.Counters);
        UAV(commands, *pool.Args);
    }
}

void GDX12ParticleSimulation::Execute(GDX12CommandList* commands, GDX12DeviceResources* resources,
    RenderPipelineCommonData& common, GDX12Texture* depth)
{
    if (!_state || !_state->Active) return;
    auto& state = *_state;
    GDX12_SCOPED_EVENT(commands, "Particle simulation", Colors::Orange);
    commands->SetComputeRootSignature(state.Root.get());
    commands->SetDescriptorHeaps({resources->SRV_UAV_Heap.get()});
    if (depth)
    {
        commands->ResourceBarrier({depth->GetResource()->GetNonPixelShaderResourceBarrier()});
        commands->SetComputeSRV(3, depth->GetSRV()->GPUHandle);
        auto& frame = resources->FrameConstants[resources->CurrFrameConstantsIndex];
        commands->SetComputeRootConstantBufferView(1, frame->CameraCB->GetElementAddress(common.ActiveCameraCBufferIndex));
    }
    for (const auto& emitter : state.Emitters)
    {
        if (emitter.Paused || state.Offloaded.count(emitter.Id)) continue;
        auto& pool = *state.Pools.at(emitter.Id);
        const uint64_t pending = emitter.BurstTotal >= pool.BurstTotal ? emitter.BurstTotal - pool.BurstTotal : 0;
        const UINT burst = static_cast<UINT>((std::min)(pending, uint64_t(emitter.MaxParticles)));
        pool.BurstTotal = emitter.BurstTotal;
        auto constants = MakeConstants(emitter, depth ? depth->GetWidth() : 1, depth ? depth->GetHeight() : 1);
        if (emitter.InteractionEnabled && _interaction.Resource)
        {
            constants.InteractionArea = _interaction.Area;
            constants.InteractionResolution = _interaction.Resolution;
            constants.InteractionFloor = _interaction.FloorCm;
            commands->ResourceBarrier({ _interaction.Resource->GetNonPixelShaderResourceBarrier() });
            commands->SetComputeSRV(0, _interaction.SRV);
        }
        pool.Constants[resources->CurrFrameConstantsIndex]->CopyData(0, constants);
        commands->SetComputeRootConstantBufferView(2, pool.Constants[resources->CurrFrameConstantsIndex]->GetElementAddress(0));
        const UINT parameters[] = { pool.Capacity, burst, state.Capacity, state.ResetMotion };
        commands->GetCommandList()->SetComputeRoot32BitConstants(0, 4, parameters, 0);
        UAV(commands, *pool.Particles);
        UAV(commands, *pool.Alive[pool.ReadIndex ^ 1]);
        UAV(commands, *pool.Dead);
        UAV(commands, *pool.Counters);
        UAV(commands, *pool.Args);
        commands->ResourceBarrier({ pool.Alive[pool.ReadIndex]->GetResource().GetNonPixelShaderResourceBarrier() });
        commands->SetComputeSRV(1, pool.Alive[pool.ReadIndex]->GetSRV()->GPUHandle);
        // t0 is unused when no field is enabled; still bind a valid read-only descriptor.
        if (!emitter.InteractionEnabled || !_interaction.Resource)
            commands->SetComputeSRV(0, pool.Alive[pool.ReadIndex]->GetSRV()->GPUHandle);
        commands->SetComputeUAV(0, pool.Particles->GetUAV()->GPUHandle);
        commands->SetComputeUAV(1, pool.Alive[pool.ReadIndex ^ 1]->GetUAV()->GPUHandle);
        commands->SetComputeUAV(2, pool.Dead->GetUAV()->GPUHandle);
        commands->SetComputeUAV(3, pool.Counters->GetUAV()->GPUHandle);
        commands->SetComputeUAV(4, pool.Args->GetUAV()->GPUHandle);
        commands->SetPipelineState(state.Compute.at("PrepareSimulation"));
        commands->Dispatch(1, 1, 1);
        UAV(commands, *pool.Counters);
        commands->ResourceBarrier({ pool.Args->GetResource().GetIndirectArgsBarrier() });
        commands->SetPipelineState(state.Compute.at("Simulate"));
        commands->ExecuteIndirect(state.DispatchSignature.Get(), 1, pool.Args->GetResource().D3DResource.Get(), 0, nullptr);
        UAV(commands, *pool.Counters);
        UAV(commands, *pool.Dead);
        UAV(commands, *pool.Particles);
        UAV(commands, *pool.Alive[pool.ReadIndex ^ 1]);
        UAV(commands, *pool.Args);
        commands->SetPipelineState(state.Compute.at("PrepareSpawn"));
        commands->Dispatch(1, 1, 1);
        UAV(commands, *pool.Counters);
        commands->ResourceBarrier({ pool.Args->GetResource().GetIndirectArgsBarrier() });
        commands->SetPipelineState(state.Compute.at("Spawn"));
        commands->ExecuteIndirect(state.DispatchSignature.Get(), 1, pool.Args->GetResource().D3DResource.Get(), 12, nullptr);
        UAV(commands, *pool.Counters);
        UAV(commands, *pool.Particles);
        UAV(commands, *pool.Args);
        commands->SetPipelineState(state.Compute.at("PublishAlive"));
        commands->Dispatch(1, 1, 1);
        UAV(commands, *pool.Counters);
        commands->ResourceBarrier({ pool.Args->GetResource().GetIndirectArgsBarrier(),
            pool.Alive[pool.ReadIndex ^ 1]->GetResource().GetNonPixelShaderResourceBarrier() });
        pool.ReadIndex ^= 1;
    }
}
