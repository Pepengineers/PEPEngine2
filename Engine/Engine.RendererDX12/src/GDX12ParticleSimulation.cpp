#include "GDX12ParticleResources.h"
#include <algorithm>
#include <stdexcept>
#include <unordered_set>

using namespace ParticleResources;

GDX12ParticleSimulation::State::State(GDX12DeviceResources* resources)
{
    Root = CreateRoot(resources);
    for (const auto* entry : { "InitializePool", "PrepareSimulation", "Simulate", "PrepareSpawn", "Spawn", "PublishAlive" })
        Compute[entry] = GDX12RenderHelpers::CreateComputePipeline(resources->Device, Root.get(),
            SHADERS_FOLDER "ParticleSimulation.hlsl", entry);
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
        if (_state) _state->Active = false;
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
    bool changed = total != state.Capacity;
    for (const auto& pool : state.Pools) changed |= ids.count(pool.first) == 0;
    for (const auto& emitter : emitters)
    {
        const auto found = state.Pools.find(emitter.Id);
        changed |= found == state.Pools.end() || found->second->Capacity != emitter.MaxParticles;
    }
    if (changed)
    {
        // All replacements happen before recording any particle work, after earlier submissions retire.
        resources->Device->GetCommandQueue()->Flush();
        for (auto it = state.Pools.begin(); it != state.Pools.end();)
            if (!ids.count(it->first)) it = state.Pools.erase(it);
            else ++it;
        for (const auto& emitter : emitters)
        {
            auto& pool = state.Pools[emitter.Id];
            if (!pool || pool->Capacity != emitter.MaxParticles)
                pool = std::make_unique<Pool>(resources, emitter.MaxParticles);
        }
        common.ResetUpscalerHistory = true;
    }
    state.Capacity = total;
    for (const auto& emitter : emitters)
    {
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
        auto& pool = *state.Pools.at(emitter.Id);
        if (pool.Initialized && pool.ResetRevision == emitter.ResetRevision) continue;
        pool.ReadIndex = 0;
        pool.BurstTotal = 0;
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
    commands->ResourceBarrier({depth->GetResource()->GetNonPixelShaderResourceBarrier()});
    commands->SetComputeSRV(3, depth->GetSRV()->GPUHandle);
    auto& frame = resources->FrameConstants[resources->CurrFrameConstantsIndex];
    commands->SetComputeRootConstantBufferView(1, frame->CameraCB->GetElementAddress(common.ActiveCameraCBufferIndex));
    for (const auto& emitter : state.Emitters)
    {
        if (emitter.Paused) continue;
        auto& pool = *state.Pools.at(emitter.Id);
        const uint64_t pending = emitter.BurstTotal >= pool.BurstTotal ? emitter.BurstTotal - pool.BurstTotal : 0;
        const UINT burst = static_cast<UINT>((std::min)(pending, uint64_t(emitter.MaxParticles)));
        pool.BurstTotal = emitter.BurstTotal;
        const auto constants = MakeConstants(emitter, depth->GetWidth(), depth->GetHeight());
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
