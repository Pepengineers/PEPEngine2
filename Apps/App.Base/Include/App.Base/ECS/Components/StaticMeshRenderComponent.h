#pragma once

#include "Engine.Core/ECS/Component.h"
#include "Engine.Core/AssetHandles.h"
#include "DirectXCollision.h"
#include <cstdint>
#include <vector>

class GDX12Material;

struct StaticMeshRenderComponent : ComponentTag
{
    Engine::Core::MeshHandle MeshHandler;
    std::vector<GDX12Material*> Materials;
    DirectX::BoundingBox Bounds;

    bool DirtyFlag;

    //we are creating an instance buffer for each submesh
    std::vector<std::uint32_t> _CBufferIndices;
    std::uint32_t _numFramesDirty;

    StaticMeshRenderComponent(Engine::Core::MeshHandle meshHandle, std::vector<GDX12Material*> materials)
        : MeshHandler(meshHandle), Materials(materials), _numFramesDirty(0), DirtyFlag(true)
    {
    }
};