#pragma once
#include "App.Base/Rendering/RenderMGPURuntime.h"
#include "Engine.RendererDX12/GDX12SnapshotTransport.h"
#include <functional>

// Synchronous graph work shares the existing bounded snapshot transport and records its actual wait cost.
class CrossAdapterWork
{
public:
    using View = GDX12Snapshot::PhysicalView;
    struct Use { View Resource; GDX12RenderGraph::Mode Mode; D3D12_RESOURCE_STATES State; GDX12Submission::TransferVersion Version{}; };
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
    explicit CrossAdapterWork(RenderMGPURuntime& runtime, const char* technique = "snapshot")
        : _runtime(runtime), _technique(technique) {}
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
    struct Profile
    {
        double RunMs = 0, TransferMs = 0, ReconfigureMs = 0, AllocateMs = 0;
        UINT64 Runs = 0, Transfers = 0, BankCreates = 0, BufferAllocations = 0;
    };
    static bool ProfilingEnabled();
    Profile GetProfile() const { return _profile; }
    struct CopyRegion
    {
        View Source, Destination;
        UINT SourceSubresource = UINT_MAX, DestinationSubresource = UINT_MAX;
        GDX12Submission::TransferVersion SourceVersion{};
        UINT64 SourceOffset = 0, DestinationOffset = 0, Bytes = 0;
    };
    UINT64 Copy(Channel& channel, GDX12Device& producer, GDX12Device& consumer,
        const std::vector<CopyRegion>& regions);
    RenderMGPURuntime::TransferCalibration Calibrate(GDX12Device& producer, GDX12Device& consumer, UINT64 bytes);
private:
    void ImportVersion(GDX12RenderGraph& graph, GDX12RenderGraph::ExecutionBindings& bindings,
        GDX12RenderGraph::Resource id, const Use& use);
    RenderMGPURuntime& _runtime;
    const char* _technique;
    Profile _profile;
};
