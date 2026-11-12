#pragma once

#include "Engine.RendererDX12/GDX12RenderGraph.h"
#include <functional>
#include <memory>
#include <optional>
#include <thread>
#include <vector>
#include <map>
#include <string>

namespace GDX12Snapshot { class Transport; }

struct RenderMGPUDevices
{
    GDX12Device* Primary = nullptr;
    GDX12Device* Secondary = nullptr;
    GDX12CommandQueue* PrimaryCopy = nullptr;
    GDX12CommandQueue* SecondaryCopy = nullptr;
};

// One render-thread submission domain. Devices and queues outlive Shutdown.
class RenderMGPURuntime final
{
public:
    using CommitReceipt = std::function<void(const GDX12RenderGraph::ExecutionReceipt&)>;

    explicit RenderMGPURuntime(RenderMGPUDevices devices);
    RenderMGPURuntime(const RenderMGPURuntime&) = delete;
    RenderMGPURuntime& operator=(const RenderMGPURuntime&) = delete;

    GDX12Submission::ResourceIdentity IdentifyResource(const std::shared_ptr<GDX12Resource>& resource, GDX12Device& device);
    struct TransferCalibration
    {
        GDX12Submission::AdapterId Producer{}, Consumer{};
        UINT64 Bytes = 0;
        double ColdMs = 0, MinimumMs = 0, MedianMs = 0, P95Ms = 0;
    };
    struct TransferLoad
    {
        UINT64 Transfers = 0, UnknownTransfers = 0;
        double OptimisticMilliseconds = 0;
    };
    void SetTransferCalibration(std::vector<TransferCalibration> samples);
    std::optional<double> EstimateTransferMilliseconds(GDX12Submission::AdapterId producer,
        GDX12Submission::AdapterId consumer, UINT64 bytes) const;
    void ObserveSynchronousTransfer(GDX12Submission::AdapterId producer, GDX12Submission::AdapterId consumer, UINT64 bytes,
        const char* technique = "snapshot");
    TransferLoad GetTransferLoad() const { return _transferLoad; }
    const std::map<std::string, TransferLoad>& GetTechniqueTransferLoads() const { return _techniqueTransferLoads; }
    void ResetTransferLoad() { _transferLoad = {}; _techniqueTransferLoads.clear(); }
    GDX12Submission::ResourceId NewResourceId();
    GDX12Submission::AllocationId NewAllocationId();
    GDX12Submission::TransferId NewTransferId();
    GDX12Submission::Epoch NewEpoch();
    GDX12CommandQueue* GetQueue(GDX12Submission::Placement placement);

    std::shared_ptr<GDX12Snapshot::Transport> CreateBank(GDX12Submission::TransferId stream,
        GDX12Submission::Epoch epoch, GDX12Submission::AdapterId producer, GDX12Submission::AdapterId consumer,
        const D3D12_RESOURCE_DESC& desc);
    bool TryReconfigureBank(GDX12Snapshot::Transport& bank, const D3D12_RESOURCE_DESC& desc);
    void RetireBank(GDX12Snapshot::Transport& bank);

    // Runtime supplies Queues. The receipt callback binds transport ownership before another submit or poll.
    std::optional<GDX12RenderGraph::ExecutionReceipt> TrySubmit(const GDX12RenderGraph& graph,
        GDX12RenderGraph::ExecutionBindings bindings, const CommitReceipt& commit = {});
    void Poll();
    size_t PendingCount() const;
    bool IsStopped() const;
    void Shutdown();

private:
    void CheckOwner() const;
    void CheckOpen() const;
    void Fault() noexcept;
    std::uint64_t NextId();
    GDX12Device* FindDevice(GDX12Submission::AdapterId id) const;

    struct Bank
    {
        GDX12Submission::TransferId Stream;
        std::shared_ptr<GDX12Snapshot::Transport> Storage;
    };
    RenderMGPUDevices _devices;
    const std::thread::id _owner;
    std::uint64_t _nextId = 1;
    bool _submitting = false;
    bool _stopped = false;
    std::vector<Bank> _banks;
    struct KnownResource { std::weak_ptr<GDX12Resource> Native; GDX12Submission::ResourceIdentity Identity; };
    std::vector<KnownResource> _resources;
    std::vector<TransferCalibration> _transferCalibration;
    TransferLoad _transferLoad;
    std::map<std::string, TransferLoad> _techniqueTransferLoads;
    GDX12RenderGraph::PlanExecutor _executor;
};
