#pragma once

#include "App.Base/Rendering/CrossAdapterSceneReplica.h"
#include "App.Base/Rendering/CrossAdapterShadows.h"

// The same depth rasterizer consumes secondary replicas or retained primary scene views.
// A Cache contains only depth produced by submitted plans for this atlas and device.
class CrossAdapterShadowRaster final
{
public:
    struct ApplicationView
    {
        CrossAdapterSceneReplica::FrameStamp Stamp;
        std::vector<std::uint64_t> Sources;
        std::vector<CrossAdapterShadows::Access> Accesses;
        std::vector<std::shared_ptr<const void>> Owners;
        std::function<void(GDX12CommandList&)> Draw;
        // The application recorder fills this only when it actually emits native draws.
        std::shared_ptr<const std::vector<std::uint64_t>> RecordedSources;
    };
    struct Group
    {
        GDX12ShadowOutput Output;
        std::optional<GDX12ShadowOutput> Static;
        UINT ReplicaGroup = UINT_MAX;
        std::array<std::optional<ApplicationView>, 6> Application;
    };
    struct GroupTrace
    {
        GDX12ShadowOutput::Kind Kind = GDX12ShadowOutput::Kind::Sun;
        GDX12LocalShadowKey Light;
        std::array<GDX12ShadowRegion, 6> CompactViews{}, PrimaryViews{};
        UINT ViewCount = 0;
        std::size_t SkinnedDraws = 0, MaskedDraws = 0;
        std::vector<std::uint64_t> AdmittedGrass, RecordedGrass;
    };
    struct SubmissionTrace
    {
        CrossAdapterSceneReplica::FrameStamp Stamp;
        GDX12Submission::CompletionPoint Finalize;
        std::vector<GroupTrace> Groups;
        std::shared_ptr<GDX12Resource> SecondaryDepth;
    };
    struct Input
    {
        GDX12Snapshot::PhysicalView Atlas;
        CrossAdapterSceneReplica::FrameStamp Stamp;
        std::shared_ptr<const CrossAdapterSceneReplica::PreparedFrame> Replica;
        // Owns the secondary upload recorder if this plan produces the replica.
        std::shared_ptr<const CrossAdapterSceneReplica::Slot> Upload;
        std::shared_ptr<GDX12DescriptorHeap> MaterialHeap;
        std::vector<Group> Groups;
        std::vector<CrossAdapterShadows::RasterStage> Preparation;
        std::vector<CrossAdapterShadows::Readiness> ExactInputs;
        bool PerFaceVisibility = false;
        std::function<void()> CommitLegacy;
        std::shared_ptr<SubmissionTrace> Trace;
        std::vector<std::uint64_t> MaskedSources;
    };

    CrossAdapterShadowRaster(GDX12Device& device, RenderMGPURuntime& runtime, GDX12ShadowAtlas atlas);
    ~CrossAdapterShadowRaster();

    GDX12Snapshot::PhysicalView CreateDepth(UINT localLayers = 0, UINT resolution = 0) const;
    bool IsReady(const Input& input, const Group& group) const;
    CrossAdapterShadows::RasterPlan Prepare(Input input);
    void InvalidateCache();
    GDX12Snapshot::PhysicalView ReuseDepth(UINT localLayers, UINT resolution = 0) const;
    GDX12ShadowAtlas Atlas() const { return _atlas; }

private:
    struct Pipeline;
    struct Cache;
    struct Work;
    GDX12Device& _device;
    RenderMGPURuntime& _runtime;
    GDX12ShadowAtlas _atlas;
    std::shared_ptr<Pipeline> _pipeline;
    std::shared_ptr<Cache> _cache;
};

std::optional<GDX12Submission::CompletionPoint> SubmitPreparedShadows(CrossAdapterShadows& transfer,
    const GDX12ShadowSelection& selection, CrossAdapterShadowRaster& primaryRaster, CrossAdapterShadowRaster::Input primary,
    CrossAdapterShadowRaster& secondaryRaster, CrossAdapterShadowRaster::Input secondary, GDX12Submission::CompletionPoint prefix);
