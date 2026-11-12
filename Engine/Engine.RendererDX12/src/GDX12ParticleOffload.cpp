#include "GDX12ParticleOffload.h"
#include "Engine.Core/Logging/Logger.h"
#include <chrono>
#include <cstring>

using namespace ParticleResources;
namespace
{
    template<class T> ComPtr<T> Open(GDX12Device& owner, T* object, GDX12Device& other)
    {
        HANDLE handle = nullptr;
        ThrowIfFailed(owner.GetDevice()->CreateSharedHandle(object, nullptr, GENERIC_ALL, nullptr, &handle));
        ComPtr<T> result;
        const auto hr = other.GetDevice()->OpenSharedHandle(handle, IID_PPV_ARGS(&result));
        CloseHandle(handle);
        ThrowIfFailed(hr);
        return result;
    }
    uint64_t Completed(ID3D12Fence* fence)
    {
        const auto result = fence->GetCompletedValue();
        if (result == UINT64_MAX) ThrowIfFailed(DXGI_ERROR_DEVICE_REMOVED);
        return result;
    }
    template<class T> std::vector<T> Read(GDX12Device& device, Buffer<T>& buffer)
    {
        auto* queue = device.GetCommandQueue();
        const auto bytes = uint64_t(buffer.GetElementCount()) * sizeof(T);
        auto readback = device.CreateResource(D3D12_HEAP_TYPE_READBACK,
            CD3DX12_RESOURCE_DESC::Buffer(bytes), D3D12_RESOURCE_STATE_COPY_DEST);
        auto& resource = buffer.GetResource();
        const auto prior = resource.GetCurrentState();
        auto* commands = queue->GetCommandList();
        commands->ResourceBarrier({resource.GetCopySourceBarrier()});
        commands->GetCommandList()->CopyBufferRegion(readback.Get(), 0, resource.D3DResource.Get(), 0, bytes);
        commands->ResourceBarrier({resource.GetTransitionBarrier(prior)});
        queue->ExecuteCommandList(commands);
        queue->CPUWaitForFenceValue(commands->FenceValue);
        void* data = nullptr;
        ThrowIfFailed(readback->Map(0, nullptr, &data));
        std::vector<T> result(buffer.GetElementCount());
        std::memcpy(result.data(), data, size_t(bytes));
        const D3D12_RANGE written{0, 0};
        readback->Unmap(0, &written);
        return result;
    }
}

GDX12ParticleOffload::GDX12ParticleOffload(GDX12DeviceResources* primary, ComPtr<IDXGIAdapter4> adapter)
    : _primary(primary)
{
    DXGI_ADAPTER_DESC1 desc{};
    ThrowIfFailed(adapter->GetDesc1(&desc));
    const auto luid = primary->Device->GetAdapterLuid();
    if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ||
        (desc.AdapterLuid.HighPart == luid.HighPart && desc.AdapterLuid.LowPart == luid.LowPart))
        throw std::invalid_argument("Particle offload requires a distinct hardware adapter");
    ThrowIfFailed(_secondary.Initialize(adapter));
    ThrowIfFailed(_secondary.GetCommandQueue()->GetCommandQueue()->SetName(L"Particle secondary solver queue"));
    _resources.Device = &_secondary;
    _resources.SRV_UAV_Heap = std::make_shared<GDX12DescriptorHeap>(&_secondary,
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 131072, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
    _resources.FrameConstants.resize(1);
    _simulation._state = std::make_unique<GDX12ParticleSimulation::State>(&_resources, true);
    _simulation._state->Active = true;
    GDX12RootSignatureDesc root;
    root.VolatileSRVData = true;
    root.Constants = {1, 1, 1, 16}; // Transfer parameters use b3; b0/b1/b2 are unused.
    root.NumSingleSRVSlots = 3; root.NumSingleUAVSlots = 2;
    _packRoot = std::make_unique<GDX12RootSignature>(&_secondary, root);
    _displayRoot = std::make_unique<GDX12RootSignature>(primary->Device, root);
    _packPSO = GDX12RenderHelpers::CreateComputePipeline(&_secondary, _packRoot.get(), SHADERS_FOLDER "ParticleTransfer.hlsl", "Pack");
    _displayPSO = GDX12RenderHelpers::CreateComputePipeline(primary->Device, _displayRoot.get(), SHADERS_FOLDER "ParticleTransfer.hlsl", "AdvanceDisplay");
    const auto flags = D3D12_FENCE_FLAG_SHARED | D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER;
    ThrowIfFailed(_secondary.GetDevice()->CreateFence(0, flags, IID_PPV_ARGS(&_readySecondary)));
    _readyPrimary = Open(_secondary, _readySecondary.Get(), *primary->Device);
    ThrowIfFailed(primary->Device->GetDevice()->CreateFence(0, flags, IID_PPV_ARGS(&_freePrimary)));
    _freeSecondary = Open(*primary->Device, _freePrimary.Get(), _secondary);
    ThrowIfFailed(primary->Device->GetDevice()->CreateFence(0, flags, IID_PPV_ARGS(&_inputReadyPrimary)));
    _inputReadySecondary = Open(*primary->Device, _inputReadyPrimary.Get(), _secondary);
    Log::Info("Particles", "Shared simulation: ", primary->Device->GetDeviceFeatures().Name,
        " <- ", _secondary.GetDeviceFeatures().Name, "; secondary LUID=", desc.AdapterLuid.HighPart, ":", desc.AdapterLuid.LowPart);
}

GDX12ParticleOffload::~GDX12ParticleOffload()
{
    Stop();
    try
    {
        _primary->Device->GetCommandQueue()->Flush();
        _secondary.GetCommandQueue()->Flush();
        Log::Info("Particles", "Offload stopped: batches=", _statistics.Steps, ", publications=", _statistics.Publications,
            ", imports=", _statistics.Imports, ", export copies=", _statistics.ExportCopies,
            ", import copies=", _statistics.ImportCopies, ", field copies=", _statistics.InputCopies);
    }
    catch (...) { OutputDebugStringA("Particle offload drain failed during shutdown\n"); }
}

GDX12ParticleOffload::Exchange GDX12ParticleOffload::CreateExchange(uint64_t bytes)
{
    const auto resource = CD3DX12_RESOURCE_DESC::Buffer(bytes, D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER);
    const auto a = _secondary.GetDevice()->GetResourceAllocationInfo(0, 1, &resource);
    const auto b = _primary->Device->GetDevice()->GetResourceAllocationInfo(0, 1, &resource);
    D3D12_HEAP_DESC heap{};
    heap.SizeInBytes = (std::max)(a.SizeInBytes, b.SizeInBytes);
    heap.Alignment = (std::max)(a.Alignment, b.Alignment);
    heap.Properties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    heap.Flags = D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER;
    Exchange result;
    ThrowIfFailed(_secondary.GetDevice()->CreateHeap(&heap, IID_PPV_ARGS(&result.SecondaryHeap)));
    result.PrimaryHeap = Open(_secondary, result.SecondaryHeap.Get(), *_primary->Device);
    ThrowIfFailed(_secondary.GetDevice()->CreatePlacedResource(result.SecondaryHeap.Get(), 0, &resource,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&result.Secondary)));
    ThrowIfFailed(_primary->Device->GetDevice()->CreatePlacedResource(result.PrimaryHeap.Get(), 0, &resource,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&result.Primary)));
    return result;
}

void GDX12ParticleOffload::Configure(const std::vector<GDX12ParticleEmitter>& emitters, UINT resolution,
    const std::unordered_map<uint64_t, uint64_t>& burstSeeds)
{
    bool changed = emitters.size() != _entries.size() || resolution != _resolution;
    for (size_t i = 0; i < emitters.size() && !changed; ++i)
        changed = emitters[i].Id != _entries[i].Id || emitters[i].ResetRevision != _entries[i].ResetRevision ||
            emitters[i].MaxParticles != _entries[i].Capacity;
    if (!changed) { Start(); return; }
    Stop();
    _primary->Device->GetCommandQueue()->Flush();
    _secondary.GetCommandQueue()->Flush();
    ++_generation;
    _resolution = resolution;
    auto oldDisplay = std::move(_display);
    const auto oldEntries = _entries;
    _entries.clear();
    if (emitters.size() > (UINT_MAX - 8) / 8) throw std::length_error("Particle metadata exceeds buffer size");
    _words = 8 + UINT(emitters.size()) * 8;
    UINT total = 0;
    auto& state = *_simulation._state;
    for (const auto& emitter : emitters)
    {
        if (emitter.MaxParticles > (UINT_MAX / 4 - _words) / 9)
            throw std::length_error("Particle payload exceeds buffer size");
        _entries.push_back({emitter.Id, emitter.ResetRevision, emitter.MaxParticles, _words, total});
        _words += emitter.MaxParticles * 9;
        total += emitter.MaxParticles;
        auto& pool = state.Pools[emitter.Id];
        if (!pool || pool->Capacity != emitter.MaxParticles)
        {
            const auto consumed = pool && pool->ResetRevision == emitter.ResetRevision ? pool->BurstTotal : 0;
            pool = std::make_unique<Pool>(&_resources, emitter.MaxParticles);
            pool->BurstTotal = consumed;
            pool->ResetRevision = emitter.ResetRevision;
        }
        const auto seed = burstSeeds.find(emitter.Id);
        if (seed != burstSeeds.end()) pool->BurstTotal = seed->second;
    }
    for (auto it = state.Pools.begin(); it != state.Pools.end();)
        if (std::none_of(emitters.begin(), emitters.end(), [&](const auto& e) { return e.Id == it->first; }))
            it = state.Pools.erase(it);
        else ++it;
    state.Capacity = total;
    _packed.reset(); _received.reset(); _display.reset(); _field.reset(); _inputField = {};
    for (auto& slot : _slots) slot = {};
    _statistics.PayloadBytes = uint64_t(_words) * 4;
    if (total)
    {
        _packed = CreateBuffer<UINT>(&_resources, _words);
        _received = CreateBuffer<UINT>(_primary, _words);
        _display = CreateBuffer<Particle>(_primary, total);
        // Range changes preserve the last shown state of unchanged emitters, never old offsets.
        auto* queue = _primary->Device->GetCommandQueue();
        auto* commands = queue->GetCommandList();
        commands->SetDescriptorHeaps({_primary->SRV_UAV_Heap.get()});
        commands->SetComputeRootSignature(_displayRoot.get());
        commands->SetPipelineState(_displayPSO);
        UAV(commands, *_display);
        commands->SetComputeUAV(1, _display->GetUAV()->GPUHandle);
        commands->ResourceBarrier({_received->GetResource().GetNonPixelShaderResourceBarrier()});
        commands->SetComputeSRV(2, _received->GetSRV()->GPUHandle);
        for (size_t i = 0; i < _entries.size(); ++i)
        {
            auto parameters = Parameters(_entries[i], i, 0);
            parameters[15] = 1;
            commands->GetCommandList()->SetComputeRoot32BitConstants(3, 16, parameters.data(), 0);
            commands->Dispatch((_entries[i].Capacity + 255) / 256, 1, 1);
        }
        if (oldDisplay)
            for (const auto& entry : _entries)
            {
                const auto old = std::find_if(oldEntries.begin(), oldEntries.end(), [&](const auto& e)
                    { return e.Id == entry.Id && e.ResetRevision == entry.ResetRevision && e.Capacity == entry.Capacity; });
                if (old == oldEntries.end()) continue;
                commands->ResourceBarrier({_display->GetResource().GetCopyDestBarrier(), oldDisplay->GetResource().GetCopySourceBarrier()});
                commands->GetCommandList()->CopyBufferRegion(_display->GetResource().D3DResource.Get(), uint64_t(entry.Base) * sizeof(Particle),
                    oldDisplay->GetResource().D3DResource.Get(), uint64_t(old->Base) * sizeof(Particle), uint64_t(entry.Capacity) * sizeof(Particle));
            }
        commands->ResourceBarrier({_display->GetResource().GetNonPixelShaderResourceBarrier()});
        queue->ExecuteCommandList(commands);
        queue->CPUWaitForFenceValue(commands->FenceValue); // Reallocation only; old display can now retire.
        for (auto& slot : _slots) static_cast<Exchange&>(slot) = CreateExchange(_statistics.PayloadBytes);
        if (resolution)
        {
            if (resolution > 4096) throw std::length_error("Particle interaction field too large");
            _field = CreateBuffer<XMFLOAT4>(&_resources, resolution * resolution);
            _inputField = CreateExchange(uint64_t(resolution) * resolution * sizeof(XMFLOAT4));
        }
        Log::Info("Particles", "Shared layout generation=", _generation, ", emitters=", emitters.size(),
            ", capacity=", total, ", bytes/publication=", _statistics.PayloadBytes, "; transport copies=1 export + 1 import");
        for (const auto& entry : _entries) Log::Info("Particles", "Offloaded emitter=", entry.Id, ", capacity=", entry.Capacity);
        Start();
    }
}

std::array<UINT, 16> GDX12ParticleOffload::Parameters(const Entry& entry, size_t index, uint64_t sequence) const
{
    return {entry.Capacity, entry.Words, 8 + UINT(index) * 8, entry.Base,
        UINT(entry.Id), UINT(entry.Id >> 32), UINT(entry.ResetRevision), UINT(entry.ResetRevision >> 32),
        UINT(_generation), UINT(_generation >> 32), UINT(sequence), UINT(sequence >> 32), _words, UINT(_entries.size()), 0, 0};
}

void GDX12ParticleOffload::Record(GDX12CommandList* commands, const std::vector<GDX12ParticleEmitter>& emitters,
    const GDX12ParticleInteractionField& interaction, bool resetMotion)
{
    std::lock_guard<std::mutex> guard(_mutex);
    if (_failure) std::rethrow_exception(_failure);
    if (_entries.empty()) return;
    GDX12_SCOPED_EVENT(commands, "Particle common import + displayed history + input snapshot", Colors::Aqua);
    Slot* source = nullptr;
    const auto ready = Completed(_readyPrimary.Get());
    for (auto& slot : _slots)
        if (slot.Published && slot.Sequence <= ready && (!source || slot.Sequence < source->Sequence)) source = &slot;
    if (source)
    {
        commands->ResourceBarrier({_received->GetResource().GetCopyDestBarrier(),
            CD3DX12_RESOURCE_BARRIER::Transition(source->Primary.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE)});
        commands->GetCommandList()->CopyBufferRegion(_received->GetResource().D3DResource.Get(), 0,
            source->Primary.Get(), 0, _statistics.PayloadBytes);
        commands->ResourceBarrier({CD3DX12_RESOURCE_BARRIER::Transition(source->Primary.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON)});
        commands->SignalOnSubmit(_freePrimary, source->Sequence);
        ++_statistics.Imports; ++_statistics.ImportCopies;
        _statistics.LastImported = source->Sequence;
        source->Published = false;
    }
    commands->SetDescriptorHeaps({_primary->SRV_UAV_Heap.get()});
    commands->SetComputeRootSignature(_displayRoot.get());
    commands->SetPipelineState(_displayPSO);
    UAV(commands, *_display);
    commands->ResourceBarrier({_received->GetResource().GetNonPixelShaderResourceBarrier()});
    commands->SetComputeSRV(2, _received->GetSRV()->GPUHandle);
    commands->SetComputeUAV(1, _display->GetUAV()->GPUHandle);
    for (size_t i = 0; i < _entries.size(); ++i)
    {
        auto parameters = Parameters(_entries[i], i, _statistics.LastImported);
        parameters[14] = source != nullptr;
        parameters[15] = resetMotion ? 2u : 0u;
        commands->GetCommandList()->SetComputeRoot32BitConstants(3, 16, parameters.data(), 0);
        commands->Dispatch((_entries[i].Capacity + 255) / 256, 1, 1);
    }
    commands->ResourceBarrier({_display->GetResource().GetNonPixelShaderResourceBarrier()});
    if (_busy || !_worker.joinable()) return;
    _job.clear();
    for (const auto& emitter : emitters)
        if (CanOffloadParticleEmitter(emitter)) _job.push_back(emitter);
    if (_job.empty()) return;
    _jobField = {};
    if (_field && interaction.Resource &&
        std::any_of(_job.begin(), _job.end(), [](const auto& emitter) { return emitter.InteractionEnabled; }))
    {
        const auto bytes = uint64_t(_resolution) * _resolution * sizeof(XMFLOAT4);
        if (interaction.Resolution != _resolution || interaction.Resource->D3DResource->GetDesc().Width < bytes)
            throw std::invalid_argument("Particle interaction snapshot changed after configuration");
        commands->ResourceBarrier({interaction.Resource->GetCopySourceBarrier(),
            CD3DX12_RESOURCE_BARRIER::Transition(_inputField.Primary.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST)});
        commands->GetCommandList()->CopyBufferRegion(_inputField.Primary.Get(), 0,
            interaction.Resource->D3DResource.Get(), 0, bytes);
        commands->ResourceBarrier({CD3DX12_RESOURCE_BARRIER::Transition(_inputField.Primary.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON),
            interaction.Resource->GetNonPixelShaderResourceBarrier()});
        _jobField = interaction;
        _jobField.Resource = &_field->GetResource();
        _jobField.SRV = _field->GetSRV()->GPUHandle;
        ++_statistics.InputCopies;
    }
    _jobNeedsInput = _jobField.Resource != nullptr;
    if (_jobNeedsInput) commands->SignalOnSubmit(_inputReadyPrimary, ++_inputSequence);
    _busy = true;
    _wake.notify_one();
}

void GDX12ParticleOffload::Simulate(const std::vector<GDX12ParticleEmitter>& emitters, GDX12ParticleInteractionField interaction)
{
    auto& state = *_simulation._state;
    state.Emitters = emitters;
    state.ResetMotion = false;
    _simulation.SetInteractionField(interaction);
    Slot* target = nullptr;
    uint64_t sequence;
    {
        std::lock_guard<std::mutex> guard(_mutex);
        const auto free = Completed(_freeSecondary.Get());
        for (size_t i = 0; i < _slots.size(); ++i)
        {
            const auto index = (_nextSlot + i) % _slots.size();
            auto& slot = _slots[index];
            if (slot.Published || free < slot.Sequence) continue;
            target = &slot;
            _nextSlot = (index + 1) % _slots.size();
            break;
        }
        sequence = _statistics.Publications + 1;
    }
    auto* queue = _secondary.GetCommandQueue();
    auto* commands = queue->GetCommandList();
    {
        GDX12_SCOPED_EVENT(commands, "Particle secondary simulation + common export", Colors::Aqua);
        if (interaction.Resource)
        {
            commands->ResourceBarrier({_field->GetResource().GetCopyDestBarrier(),
                CD3DX12_RESOURCE_BARRIER::Transition(_inputField.Secondary.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE)});
            commands->GetCommandList()->CopyBufferRegion(_field->GetResource().D3DResource.Get(), 0,
                _inputField.Secondary.Get(), 0, uint64_t(_resolution) * _resolution * sizeof(XMFLOAT4));
            commands->ResourceBarrier({CD3DX12_RESOURCE_BARRIER::Transition(_inputField.Secondary.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON)});
        }
        _simulation.InitializePools(commands, &_resources);
        _simulation.Execute(commands, &_resources, _common, nullptr);
        if (target)
        {
            commands->SetDescriptorHeaps({_resources.SRV_UAV_Heap.get()});
            commands->SetComputeRootSignature(_packRoot.get());
            commands->SetPipelineState(_packPSO);
            UAV(commands, *_packed);
            commands->SetComputeUAV(0, _packed->GetUAV()->GPUHandle);
            for (size_t i = 0; i < _entries.size(); ++i)
            {
                const auto& entry = _entries[i];
                auto& pool = *state.Pools.at(entry.Id);
                commands->ResourceBarrier({pool.Particles->GetResource().GetNonPixelShaderResourceBarrier(),
                    pool.Counters->GetResource().GetNonPixelShaderResourceBarrier()});
                commands->SetComputeSRV(0, pool.Particles->GetSRV()->GPUHandle);
                commands->SetComputeSRV(1, pool.Counters->GetSRV()->GPUHandle);
                const auto parameters = Parameters(entry, i, sequence);
                commands->GetCommandList()->SetComputeRoot32BitConstants(3, 16, parameters.data(), 0);
                commands->Dispatch((entry.Capacity + 255) / 256, 1, 1);
            }
            commands->ResourceBarrier({_packed->GetResource().GetCopySourceBarrier(),
                CD3DX12_RESOURCE_BARRIER::Transition(target->Secondary.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST)});
            commands->GetCommandList()->CopyBufferRegion(target->Secondary.Get(), 0, _packed->GetResource().D3DResource.Get(), 0, _statistics.PayloadBytes);
            commands->ResourceBarrier({CD3DX12_RESOURCE_BARRIER::Transition(target->Secondary.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON)});
        }
    }
    queue->ExecuteCommandList(commands);
    if (target) ThrowIfFailed(queue->GetCommandQueue()->Signal(_readySecondary.Get(), sequence));
    queue->CPUWaitForFenceValue(queue->Signal()); // Worker only; bounds uploads and secondary submissions.
    {
        std::lock_guard<std::mutex> guard(_mutex);
        ++_statistics.Steps;
        if (target)
        {
            target->Sequence = sequence;
            target->Published = true;
            ++_statistics.Publications; ++_statistics.ExportCopies;
            ++_statistics.SlotPublications[size_t(target - _slots.data())];
        }
        _busy = false;
    }
}

void GDX12ParticleOffload::Run()
{
    try
    {
        for (;;)
        {
            std::vector<GDX12ParticleEmitter> emitters;
            GDX12ParticleInteractionField field;
            {
                std::unique_lock<std::mutex> guard(_mutex);
                _wake.wait(guard, [&] { return _stop || _busy; });
                if (_stop) return;
                if (_jobNeedsInput && Completed(_inputReadySecondary.Get()) < _inputSequence)
                {
                    _wake.wait_for(guard, std::chrono::milliseconds(1));
                    continue;
                }
                emitters = _job;
                field = _jobField;
            }
            Simulate(emitters, field);
        }
    }
    catch (...)
    {
        std::lock_guard<std::mutex> guard(_mutex);
        _failure = std::current_exception();
        _busy = false;
    }
}

void GDX12ParticleOffload::Start()
{
    if (_worker.joinable() || _entries.empty()) return;
    _stop = false;
    _worker = std::thread([this] { Run(); });
}
void GDX12ParticleOffload::Stop()
{
    { std::lock_guard<std::mutex> guard(_mutex); _stop = true; }
    _wake.notify_one();
    if (_worker.joinable()) _worker.join();
    _busy = false;
}
bool GDX12ParticleOffload::Find(uint64_t id, UINT& base) const
{
    for (const auto& entry : _entries) if (entry.Id == id) { base = entry.Base; return true; }
    return false;
}
uint64_t GDX12ParticleOffload::ConsumedBurst(uint64_t id) const
{
    if (_worker.joinable()) throw std::logic_error("Stop particle worker before inspecting pool state");
    const auto found = _simulation._state->Pools.find(id);
    return found == _simulation._state->Pools.end() ? 0 : found->second->BurstTotal;
}
std::vector<GDX12Resource*> GDX12ParticleOffload::Resources() const
{
    if (!_display) return {};
    return {&_display->GetResource(), &_received->GetResource()};
}
GDX12ParticleOffload::Statistics GDX12ParticleOffload::GetStatistics() const
{
    std::lock_guard<std::mutex> guard(_mutex);
    return _statistics;
}
std::vector<UINT> GDX12ParticleOffload::ReadPayload(bool sent)
{
    Stop();
    return sent ? Read(_secondary, *_packed) : Read(*_primary->Device, *_received);
}
std::vector<Particle> GDX12ParticleOffload::ReadSolver(uint64_t id)
{
    Stop();
    return Read(_secondary, *_simulation._state->Pools.at(id)->Particles);
}
