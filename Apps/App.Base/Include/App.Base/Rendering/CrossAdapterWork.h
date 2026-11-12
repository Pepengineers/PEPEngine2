#pragma once
#include "App.Base/Rendering/RenderMGPURuntime.h"
#include "Engine.RendererDX12/GDX12SnapshotTransport.h"
#include <functional>

// Synchronous graph work shares the existing bounded snapshot transport and records its actual wait cost.
class CrossAdapterWork
{
public:
    using View = GDX12Snapshot::PhysicalView;
    struct Use { View Resource; GDX12RenderGraph::Mode Mode; D3D12_RESOURCE_STATES State; };
    using Record = std::function<void(GDX12CommandList&)>;
    struct Channel
    {
        GDX12Submission::TransferId Id{};
        GDX12Submission::Epoch EpochId{};
        std::shared_ptr<GDX12Snapshot::Transport> Bank;
        View Source;
        UINT64 Bytes = 0, Invocation = 0;
        GDX12Submission::AdapterId Producer{}, Consumer{};
    };
    explicit CrossAdapterWork(RenderMGPURuntime& runtime) : _runtime(runtime) {}
    View Identify(std::shared_ptr<GDX12Resource> resource, GDX12Device& device);
    View Allocate(GDX12Device& device, UINT64 bytes);
    void Wait(GDX12Submission::CompletionPoint point);
    GDX12Submission::CompletionPoint Run(const char* name, GDX12Device& device,
        std::vector<Use> uses, Record record);
    GDX12Submission::CompletionPoint Transfer(Channel& channel, GDX12Device& producer, GDX12Device& consumer,
        UINT64 bytes, std::vector<Use> source, std::vector<Use> destination,
        std::function<void(GDX12CommandList&, GDX12Resource&)> pack,
        std::function<void(GDX12CommandList&, GDX12Resource&)> unpack);
    double WaitMilliseconds = 0;
    struct CopyRegion
    {
        View Source, Destination;
        UINT SourceSubresource = UINT_MAX, DestinationSubresource = UINT_MAX;
    };
    UINT64 Copy(Channel& channel, GDX12Device& producer, GDX12Device& consumer,
        const std::vector<CopyRegion>& regions);
private:
    RenderMGPURuntime& _runtime;
    struct Identity { std::weak_ptr<GDX12Resource> Native; GDX12Submission::ResourceIdentity Id; };
    std::vector<Identity> _identities;
};
