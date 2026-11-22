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
    void LogFirstImport(const char* technique, CompletionToken source, CompletionToken imported) const;
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
    CompletionToken Submit(Id, BistroRender::Queue, const std::vector<ID3D12CommandList*>&,
        const std::vector<CompletionToken>&, GDX12CommandList* external = nullptr);
    const std::vector<DeviceDesc> _adapters;
    const int _primaryIndex;
    std::vector<std::string> _adapterLabels;
    std::vector<std::unique_ptr<DeviceContext>> _devices;
    std::vector<bool> _unavailable;
    std::map<std::tuple<Id, Id, size_t>, ComPtr<ID3D12Fence>> _peerFences;
    uint64_t _incarnation;
    mutable std::mutex _mutex;
    mutable std::atomic<bool> _quarantined{false};
};
}
