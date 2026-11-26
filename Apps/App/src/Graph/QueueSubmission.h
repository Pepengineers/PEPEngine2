#pragma once
#include "RenderGraph.h"
#include "GraphResourceInfo.h"
#include "Engine.RendererDX12/GDX12Device.h"
#include "Engine.RendererDX12/GDX12DeviceFactory.h"
#include <array>
#include <memory>
#include <mutex>
#include <atomic>
#include <map>
#include <tuple>
class GDX12CommandQueue;
class GDX12CommandList;
namespace BistroRender {
struct CompletionToken {
    Id Device = Invalid;
    Queue Target = Queue::Direct;
    uint64_t Value = 0, Incarnation = 0;
    explicit operator bool() const { return Value != 0; }
};
struct PacketWait { Id Packet; CompletionToken Token; };

// One submission owner for all recording contexts. Tokens are capabilities for
// already-issued signals, never promises of a future recording/submission.
class QueueSubmission {
public:
    QueueSubmission(GDX12Device& primary, std::vector<DeviceDesc> adapters, int primaryIndex);
    ~QueueSubmission();
    GDX12Device& Device(Id) const;
    Id SelectAdapter(int hardwareIndex);
    const std::string& AdapterLabel(Id) const;
    LUID AdapterLuid(Id) const;
    void LogFirstImport(const char* technique, CompletionToken source, CompletionToken imported) const;
    struct BenchmarkWait { Id From, To; Queue SourceQueue, TargetQueue; uint64_t FirstFence, Count; };
    struct BenchmarkPacket
    {
        std::string Name;
        CompletionToken Token;
        CompletionToken Terminal;
        std::vector<CompletionToken> Dependencies;
        double BeginMs = 0, EndMs = 0, SubmitMs = 0;
        uint64_t SharedBytes = 0;
        bool Calibrated = false;
    };
    void BeginBenchmarkWaits();
    std::vector<BenchmarkWait> EndBenchmarkWaits();
    uint64_t BenchmarkEpoch() const { return _benchmarkEpoch.load(std::memory_order_relaxed); }
    std::vector<BenchmarkPacket> TakeBenchmarkPackets();
    struct PacketInterval { Id Device; double BeginMs, EndMs; };
    std::vector<PacketInterval> ReadBenchmarkIntervals(uint64_t& epoch, size_t& cursor, bool consume);
    std::vector<int> BenchmarkAdapterIndices(bool includeUnavailable = false) const;
    uint64_t Incarnation() const { return _incarnation; }
    struct TransferBuffer {
        std::array<ComPtr<ID3D12Heap>, 2> Heaps;
        std::array<ComPtr<ID3D12Resource>, 2> Resources;
    };
    TransferBuffer CreateTransferBuffer(Id remote, uint64_t bytes) const;
    TransferBuffer CreateTransferBuffer(Id first, Id second, uint64_t bytes) const;
    void CheckHealth() const;
    bool Complete(CompletionToken) const;
    void NotifyOnCompletion(CompletionToken, HANDLE event) const;
    void Wait(CompletionToken) const; // drain/retirement only; never version selection
    GraphResourceInfo ValidateResource(Id, ID3D12Resource*) const;
private:
    friend class Executor;
    struct BenchmarkSlot
    {
        struct Timing
        {
            ComPtr<ID3D12QueryHeap> Heap;
            GDX12ResourceHandle Readback;
            uint64_t Frequency = 0, AnchorGpu = 0, AnchorCpu = 0;
            UINT Capacity = 0, Count = 0;
            bool Calibrated = false;
        };
        // Held while recording or resolving this slot. Independent GPU producers have separate slots.
        std::mutex Mutex;
        std::array<Timing, 3> Timings;
        std::vector<BenchmarkPacket> Samples;
        uint64_t Epoch = 0;
    };
    bool TrackBenchmarkSlot(const std::shared_ptr<BenchmarkSlot>& slot, uint64_t epoch);
    void CollectBenchmarkSlot(BenchmarkSlot& slot, bool waitForGpu);
    struct Timeline {
        std::unique_ptr<GDX12CommandQueue> Owned;
        GDX12CommandQueue* Queue = nullptr;
        ComPtr<ID3D12Fence> Shared, OnPrimary, PrimaryOnRemote;
        uint64_t Issued = 0;
        bool Poisoned = false;
    };
    struct DeviceContext {
        DeviceContext(GDX12Device&, std::unique_ptr<GDX12Device> owned = {});
        std::unique_ptr<GDX12Device> Owned;
        GDX12Device& Device;
        ComPtr<ID3D12Device> Identity;
        ComPtr<IUnknown> ComIdentity;
        std::array<Timeline, 3> Queues;
    };
    void Check() const;
    const Timeline& Resolve(CompletionToken) const;
    ID3D12Fence* CompletionFence(CompletionToken) const;
    CompletionToken Submit(Id, BistroRender::Queue, const std::vector<ID3D12CommandList*>* packets, size_t count,
        const std::vector<CompletionToken>&, GDX12CommandList* external = nullptr, uint64_t benchmarkEpoch = 0);
    const std::vector<DeviceDesc> _adapters;
    const int _primaryIndex;
    std::vector<std::string> _adapterLabels;
    std::vector<std::unique_ptr<DeviceContext>> _devices;
    std::vector<bool> _unavailable;
    std::map<std::tuple<Id, Id, size_t>, ComPtr<ID3D12Fence>> _peerFences;
    uint64_t _incarnation;
    mutable std::mutex _mutex;
    uint64_t _nextBenchmarkEpoch = 0;
    std::atomic<uint64_t> _benchmarkEpoch{0};
    std::vector<BenchmarkWait> _benchmarkWaits;
    std::vector<BenchmarkPacket> _benchmarkPackets;
    std::vector<std::shared_ptr<BenchmarkSlot>> _benchmarkSlots;
    mutable std::atomic<bool> _quarantined{false};
};
}
