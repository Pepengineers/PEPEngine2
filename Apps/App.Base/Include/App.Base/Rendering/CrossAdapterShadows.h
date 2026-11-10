#pragma once

#include "App.Base/Rendering/RenderMGPURuntime.h"
#include "Engine.RendererDX12/GDX12ShadowDepthTransfer.h"
#include "Engine.RendererDX12/GDX12SnapshotTransport.h"
#include <array>

bool IsMultiGPUShadowsEnabled();

class CrossAdapterShadows final
{
public:
    struct Selection
    {
        GDX12ShadowTransferLayout Layout;
        std::vector<UINT> SecondaryGroups;
        std::vector<UINT> PrimaryGroups;
        // Same ordering as SecondaryGroups, with compact local destinations.
        std::vector<GDX12ShadowOutput> SecondaryOutput;
    };

    struct Access
    {
        GDX12Snapshot::PhysicalView View;
        GDX12RenderGraph::Mode Mode;
        D3D12_RESOURCE_STATES State;
        GDX12Submission::TransferVersion Version{};
    };
    struct Readiness
    {
        GDX12Snapshot::PhysicalView View;
        GDX12Submission::TransferVersion Version;
        GDX12Submission::CompletionPoint Ready;
        std::shared_ptr<const void> Owner;
    };
    struct RasterStage
    {
        const char* Name;
        std::vector<Access> Accesses;
        std::function<void(GDX12CommandList&)> Record;
    };
    struct RasterPlan
    {
        std::vector<RasterStage> Stages;
        std::vector<Readiness> ExactInputs;
        std::vector<std::shared_ptr<const void>> Owners;
        std::function<void()> Commit;
    };

    CrossAdapterShadows(GDX12Device& primary, GDX12Device& secondary, RenderMGPURuntime& runtime);
    ~CrossAdapterShadows();

    GDX12ShadowSelection ReadSelection(const std::vector<GDX12ShadowOutput>& preparedLocal);
    static std::optional<Selection> Select(GDX12ShadowAtlas atlas, const GDX12ShadowSelection& selection,
        const std::vector<GDX12ShadowOutput>& prepared, const std::vector<bool>& replicaReady);

    // A missing receipt permits legacy fallback only when no placed submit occurred.
    std::optional<GDX12Submission::CompletionPoint> Submit(const Selection& selection,
        const std::vector<GDX12ShadowOutput>& primaryPrepared, const GDX12Snapshot::PhysicalView& primaryAtlas,
        const GDX12Snapshot::PhysicalView& secondaryAtlas, const RasterPlan& primary,
        const RasterPlan& secondary, GDX12Submission::CompletionPoint prefix);
    void RetireFrame(GDX12Submission::CompletionPoint tail);
    void RetireAtlas(GDX12ShadowAtlas atlas);

private:
    struct Stream;
    GDX12Device& _primary;
    GDX12Device& _secondary;
    RenderMGPURuntime& _runtime;
    std::array<std::unique_ptr<Stream>, 2> _streams;
    std::shared_ptr<GDX12ShadowDepthTransfer> _pack, _restore;
    GDX12LocalShadowKey _chosenLight;
    int _chosenMode = -1;
    std::int64_t _chosenSlot = -1;
    std::uint64_t _invocation = 0;
};
