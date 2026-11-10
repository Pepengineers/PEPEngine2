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
    };
    struct Group
    {
        GDX12ShadowOutput Output;
        std::optional<GDX12ShadowOutput> Static;
        UINT ReplicaGroup = UINT_MAX;
        std::array<std::optional<ApplicationView>, 6> Application;
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
    };

    CrossAdapterShadowRaster(GDX12Device& device, RenderMGPURuntime& runtime, GDX12ShadowAtlas atlas);
    ~CrossAdapterShadowRaster();

    GDX12Snapshot::PhysicalView CreateDepth(UINT localLayers = 0) const;
    bool IsReady(const Input& input, const Group& group) const;
    CrossAdapterShadows::RasterPlan Prepare(Input input);
    void InvalidateCache();
    GDX12Snapshot::PhysicalView ReuseDepth(UINT localLayers) const;
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
