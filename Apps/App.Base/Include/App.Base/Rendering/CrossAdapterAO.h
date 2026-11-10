#pragma once

#include "App.Base/Rendering/RenderMGPURuntime.h"
#include "Engine.RendererDX12/GDX12SnapshotTransport.h"
#include "Engine.RendererDX12/RenderPasses/GDX12AmbientOcclusionPass.h"
#include <array>
#include <unordered_map>

bool IsMultiGPUAmbientOcclusionEnabled();

// Replaces only the legacy graph node. Its links, local renderer and history remain owned by AO.
class CrossAdapterAO final : public GDX12RenderPass
{
public:
    CrossAdapterAO(GDX12AmbientOcclusionPass& local, GDX12DeviceResources& primary, GDX12Device& secondary, RenderMGPURuntime& runtime);
    ~CrossAdapterAO() override;

    void RecordRenderGraph(GDX12RenderGraphBuilder& graph) override { _local.RecordRenderGraph(graph); }
    bool IsAsyncComputeEnabled() const override { return _local.IsAsyncComputeEnabled(); }
    std::vector<GDX12RenderGraph::BufferAccess> GetBufferAccesses() const override { return _local.GetBufferAccesses(); }
    void Execute(GDX12CommandList* commands) override;

    std::optional<GDX12Submission::CompletionPoint> Submit(const GDX12RenderGraph::LegacyBoundary& boundary,
        GDX12Submission::CompletionPoint prefix);
    void RetireFrame(GDX12Submission::CompletionPoint tail);
    GDX12Submission::CompletionPoint GetLastFinalize() const { return _lastFinalize; }
#ifdef _DEBUG
    struct DebugFrame
    {
        GDX12AmbientOcclusionPass::FrameInput Input;
        std::array<std::weak_ptr<GDX12Resource>, 3> Source, Imported;
    };
    const DebugFrame& GetDebugFrame() const { return _debugFrame; }
#endif

private:
    struct Frame;
    struct BoundaryIdentity
    {
        std::weak_ptr<GDX12Resource> Native;
        GDX12Submission::ResourceIdentity Identity;
    };

    bool PrepareLayout(const GDX12AmbientOcclusionPass::FrameInput& input);
    GDX12Snapshot::PhysicalView BindBoundary(const GDX12RenderGraph::LegacyBoundaryView& view);

    GDX12AmbientOcclusionPass& _local;
    GDX12DeviceResources& _primary;
    RenderMGPURuntime& _runtime;
    GDX12DeviceResources _secondary;
    GDX12AmbientOcclusionPass _evaluate;
    bool _secondaryReady = false;
    bool _loggedFallback = false;
    std::optional<GDX12AmbientOcclusionPass::FrameInput> _frozen;
    std::array<GDX12Submission::TransferId, 3> _streams{};
    std::array<std::shared_ptr<GDX12Snapshot::Transport>, 3> _banks;
    std::array<bool, 3> _configured{};
    std::array<D3D12_RESOURCE_DESC, 3> _layout{};
    GDX12Submission::Epoch _epoch = GDX12Submission::Epoch::Invalid;
    std::uint64_t _invocation = 0;
    UINT _width = 0, _height = 0;
    GDX12AOResolution _resolution = GDX12AOResolution::Full;
    std::unordered_map<GDX12Resource*, BoundaryIdentity> _boundaryIds;
    std::array<std::shared_ptr<Frame>, GDX12Snapshot::SlotCount> _frames;
    Frame* _awaitingTail = nullptr;
    GDX12Submission::CompletionPoint _lastFinalize;
#ifdef _DEBUG
    DebugFrame _debugFrame{};
#endif
};
