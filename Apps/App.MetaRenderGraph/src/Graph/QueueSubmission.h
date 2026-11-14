#pragma once
#include "MetaGraph.h"
#include "Engine.RendererDX12/GDX12Device.h"
#include <array>
#include <memory>
#include <mutex>
#include <atomic>
class GDX12CommandQueue;
class GDX12CommandList;
namespace Meta {
struct CompletionToken {
    Id Device = Invalid;
    Queue Target = Queue::Direct;
    uint64_t Value = 0, Incarnation = 0;
    explicit operator bool() const { return Value != 0; }
};
struct PacketWait { Id Packet; CompletionToken Token; };

// One submission owner for both recording contexts. Tokens are capabilities for
// already-issued signals, never promises of a future recording/submission.
class QueueSubmission {
public:
    explicit QueueSubmission(GDX12Device& primary, GDX12Device* secondary = nullptr);
    ~QueueSubmission();
    GDX12Device& Device(Id) const;
    uint64_t Incarnation() const { return _incarnation; }
    void CheckHealth() const;
    bool Complete(CompletionToken) const;
    void Wait(CompletionToken) const; // drain/retirement only; never version selection
    void ValidateResource(Id, ID3D12Resource*) const;
private:
    friend class Executor;
    struct Timeline {
        std::unique_ptr<GDX12CommandQueue> Owned;
        GDX12CommandQueue* Queue = nullptr;
        ComPtr<ID3D12Fence> Shared, Remote;
        uint64_t Issued = 0;
        bool Poisoned = false;
    };
    void Check() const;
    const Timeline& Resolve(CompletionToken) const;
    CompletionToken Submit(Id, Meta::Queue, const std::vector<ID3D12CommandList*>&,
        const std::vector<CompletionToken>&, GDX12CommandList* external = nullptr);
    std::array<GDX12Device*,2> _devices{};
    std::array<ComPtr<ID3D12Device>,2> _identities;
    std::array<std::array<Timeline,3>,2> _queues;
    uint64_t _incarnation;
    mutable std::mutex _mutex;
    mutable std::atomic<bool> _quarantined{false};
};
}
