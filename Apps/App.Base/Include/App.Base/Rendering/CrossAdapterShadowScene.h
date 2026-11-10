#pragma once

#include "App.Base/Rendering/CrossAdapterSceneReplica.h"
#include "Engine.RendererDX12/GDX12Skinning.h"

class RenderMGPURuntime;

struct FrozenShadowSkin
{
    UINT CasterIndex = UINT_MAX;
    Engine::Core::MeshHandle Mesh;
    std::uint64_t ContentRevision = 0;
    UINT Submesh = 0;
    const Engine::Core::SubMesh* Source = nullptr;
    std::vector<GDX12SkinPalette> Palette;
};

struct ShadowSceneSources
{
    // Indexed by the snapshot's logical mesh and alpha indices. Borrowed for Prepare only.
    std::vector<const Engine::Core::SubMesh*> Meshes;
    std::vector<const CrossAdapterSceneReplica::AlphaTexture*> Alpha;
    std::vector<FrozenShadowSkin> Skin;
};

struct PreparedShadowReplica
{
    struct Stage
    {
        const char* Name;
        std::vector<CrossAdapterSceneReplica::Access> Accesses;
        std::function<void(GDX12CommandList&)> Record;
    };

    std::shared_ptr<const CrossAdapterSceneReplica::PreparedFrame> Replica;
    std::shared_ptr<const CrossAdapterSceneReplica::Slot> Upload;
    std::shared_ptr<GDX12DescriptorHeap> MaterialHeap;
    std::vector<Stage> Preparation;
    // Invoke once from the successful RasterPlan receipt on the render owner thread.
    std::function<void()> Commit;
};

class CrossAdapterShadowScene
{
public:
    CrossAdapterShadowScene(RenderMGPURuntime& runtime, GDX12Device& secondary);
    CrossAdapterShadowScene(GDX12Device& secondary, CrossAdapterSceneReplica::NewIdentity newIdentity);
    CrossAdapterShadowScene(const CrossAdapterShadowScene&) = delete;
    CrossAdapterShadowScene& operator=(const CrossAdapterShadowScene&) = delete;

    // Run after executor collection, even when no atlas will prepare this frame.
    void CollectRetired();

    // An empty result means all slots are retained by work awaiting its last secondary reader.
    PreparedShadowReplica Prepare(const CrossAdapterSceneReplica::Snapshot& snapshot, const ShadowSceneSources& sources);

private:
    struct State;
    std::shared_ptr<State> _state;
};
