#pragma once

#include "Engine.Core/ECS/Component.h"
#include "Engine.Core/AssetHandles.h"
#include "DirectXCollision.h"
#include <cstdint>
#include <vector>

class GDX12Material;

struct StaticMeshRenderComponent : ComponentTag
{
    // Call MarkComponentUpdated after changing the mesh or its material slots.
    Engine::Core::MeshHandle MeshHandler;
    std::vector<GDX12Material*> Materials;
    // Set DirtyFlag or call MarkComponentUpdated after changing CastShadows.
    bool CastShadows = true;
    bool DirtyFlag;

    StaticMeshRenderComponent(Engine::Core::MeshHandle meshHandle, std::vector<GDX12Material*> materials)
        : MeshHandler(meshHandle), Materials(materials), DirtyFlag(true)
    {
    }

private:
    friend class RenderModule;
    friend class GPUDataUpdateSystem;

    DirectX::BoundingBox Bounds;
    std::vector<std::uint32_t> _CBufferIndices;
    // Last mesh installed in the draw cache. Compared only when the component update event arrives.
    Engine::Core::MeshHandle _boundMeshHandler;
    // Initial placement is static; the first later world-matrix change makes it dynamic permanently.
    bool _shadowCasterInitialized = false;
    bool _shadowCasterDynamic = false;
    std::uint32_t _numFramesDirty = 0;
};
