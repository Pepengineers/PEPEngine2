#include "App.Base/Rendering/RenderMGPURuntime.h"
#include "Engine.RendererDX12/GDX12CommandQueue.h"
#include "Engine.RendererDX12/GDX12SnapshotTransport.h"
#include <algorithm>
#include <stdexcept>

using namespace GDX12Submission;

namespace
{
    GDX12Snapshot::PoolIds NewPoolIds(RenderMGPURuntime& runtime)
    {
        GDX12Snapshot::PoolIds ids{};
        for (auto& slot : ids)
            slot = {runtime.NewResourceId(), runtime.NewResourceId(), runtime.NewResourceId(),
                runtime.NewAllocationId(), runtime.NewAllocationId()};
        return ids;
    }
}

RenderMGPURuntime::RenderMGPURuntime(RenderMGPUDevices devices)
    : _devices(devices), _owner(std::this_thread::get_id()), _executor(16)
{
    if (!devices.Primary || !devices.Secondary || devices.Primary == devices.Secondary ||
        !devices.PrimaryCopy || !devices.SecondaryCopy)
        throw std::invalid_argument("MGPU runtime requires two devices and their COPY queues");
}

void RenderMGPURuntime::CheckOwner() const
{
    if (std::this_thread::get_id() != _owner || _submitting)
        throw std::logic_error("MGPU runtime requires its serialized render owner outside receipt callbacks");
}

void RenderMGPURuntime::CheckOpen() const
{
    CheckOwner();
    if (_stopped) throw std::logic_error("MGPU submission domain stopped");
}

std::uint64_t RenderMGPURuntime::NextId()
{
    CheckOpen();
    if (_nextId == UINT64_MAX) throw std::overflow_error("MGPU runtime identities exhausted");
    return _nextId++;
}

ResourceId RenderMGPURuntime::NewResourceId() { return ResourceId{NextId()}; }
AllocationId RenderMGPURuntime::NewAllocationId() { return AllocationId{NextId()}; }
TransferId RenderMGPURuntime::NewTransferId() { return TransferId{NextId()}; }
Epoch RenderMGPURuntime::NewEpoch() { return Epoch{NextId()}; }

GDX12Device* RenderMGPURuntime::FindDevice(AdapterId id) const
{
    if (id == _devices.Primary->GetSubmissionAdapterId()) return _devices.Primary;
    if (id == _devices.Secondary->GetSubmissionAdapterId()) return _devices.Secondary;
    return nullptr;
}

GDX12CommandQueue* RenderMGPURuntime::GetQueue(Placement placement)
{
    CheckOpen();
    auto* device = FindDevice(placement.Adapter);
    if (!device || !placement.IsValid()) return nullptr;
    switch (placement.Queue)
    {
    case QueueKind::Direct: return device->GetCommandQueue();
    case QueueKind::Compute: return device->TryGetComputeQueue();
    case QueueKind::Copy: return device == _devices.Primary ? _devices.PrimaryCopy : _devices.SecondaryCopy;
    }
    return nullptr;
}

std::shared_ptr<GDX12Snapshot::Transport> RenderMGPURuntime::CreateBank(TransferId stream, Epoch epoch,
    AdapterId producer, AdapterId consumer, const D3D12_RESOURCE_DESC& desc)
{
    CheckOpen();
    auto* from = FindDevice(producer);
    auto* to = FindDevice(consumer);
    if (!from || !to || from == to || stream == TransferId::Invalid || epoch == Epoch::Invalid)
        throw std::invalid_argument("Invalid MGPU bank identity or adapter pair");
    if (std::any_of(_banks.begin(), _banks.end(), [stream](const Bank& bank) { return bank.Stream == stream; }))
        throw std::invalid_argument("MGPU stream already has a bank");
    auto storage = std::make_shared<GDX12Snapshot::Transport>(*from, *to,
        *GetQueue({producer, QueueKind::Copy}), *GetQueue({consumer, QueueKind::Copy}), stream, epoch, desc,
        NewPoolIds(*this));
    _banks.push_back({stream, storage});
    return storage;
}

bool RenderMGPURuntime::TryReconfigureBank(GDX12Snapshot::Transport& bank, const D3D12_RESOURCE_DESC& desc)
{
    CheckOpen();
    if (std::none_of(_banks.begin(), _banks.end(), [&](const Bank& entry) { return entry.Storage.get() == &bank; }))
        throw std::invalid_argument("Cannot reconfigure a bank outside its runtime domain");
    if (bank.GetUsage().Publications != 0) return false;
    return bank.TryReconfigure(desc, NewPoolIds(*this));
}

std::optional<GDX12RenderGraph::ExecutionReceipt> RenderMGPURuntime::TrySubmit(const GDX12RenderGraph& graph,
    GDX12RenderGraph::ExecutionBindings bindings, const CommitReceipt& commit)
{
    CheckOpen();
    if (!bindings.Queues.empty()) throw std::invalid_argument("MGPU runtime owns native queue bindings");
    for (const auto& batch : graph.GetCompiledPlan().Batches)
    {
        auto* queue = GetQueue(batch.Location);
        if (!queue) throw std::invalid_argument("MGPU plan placement is unavailable");
        if (std::none_of(bindings.Queues.begin(), bindings.Queues.end(),
            [queue](const GDX12RenderGraph::QueueBinding& binding) { return binding.Queue == queue; }))
            bindings.Queues.push_back({FindDevice(batch.Location.Adapter), queue});
    }
    for (const auto& input : bindings.ExactInputs)
    {
        auto* issuer = GetQueue(input.Ready.Timeline().Producer.Location);
        if (!issuer || !(issuer->GetTimelineIdentity() == input.Ready.Timeline()) ||
            (input.SharedProducer && input.SharedProducer != issuer))
            throw std::invalid_argument("MGPU input readiness is outside the runtime domain");
    }

    _submitting = true;
    bool issued = false;
    try
    {
        auto receipt = _executor.TrySubmit(graph, bindings);
        issued = receipt.has_value();
        if (receipt && commit) commit(*receipt);
        _submitting = false;
        return receipt;
    }
    catch (...)
    {
        _submitting = false;
        if (issued || _executor.IsSubmissionFaulted()) Fault();
        throw;
    }
}

void RenderMGPURuntime::Fault() noexcept
{
    _stopped = true;
    for (const auto& bank : _banks) bank.Storage->FaultSubmission();
}

void RenderMGPURuntime::Poll()
{
    CheckOwner();
    if (_stopped) return;
    try
    {
        _executor.CollectCompleted();
        for (const auto& bank : _banks) bank.Storage->Poll();
    }
    catch (...)
    {
        Fault();
        throw;
    }
}

size_t RenderMGPURuntime::PendingCount() const
{
    CheckOwner();
    return _executor.PendingCount();
}

bool RenderMGPURuntime::IsStopped() const
{
    CheckOwner();
    return _stopped;
}

void RenderMGPURuntime::Shutdown()
{
    CheckOwner();
    _stopped = true;
    _executor.Drain();
    for (const auto& bank : _banks) bank.Storage->Shutdown();
    _banks.clear();
}
