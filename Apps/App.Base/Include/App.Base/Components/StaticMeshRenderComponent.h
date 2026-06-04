#pragma once

#include "Component.h"
#include "Engine.RendererDX12/D3DHelpers.h"
#include "Engine.Core/AssetHandles.h"
#include "DirectXCollision.h"
#include "Engine.RendererDX12/GDX12Material.h"

struct StaticMeshRenderComponent : ComponentTag
{
    Engine::Core::MeshHandle MeshHandler;
    std::vector<GDX12Material*> Materials;
    BoundingBox Bounds;

    bool DirtyFlag;

    //we are creating an instance buffer for each submesh
    std::vector<UINT> _CBufferIndices;
    UINT _numFramesDirty;

    StaticMeshRenderComponent(Engine::Core::MeshHandle meshHandle, std::vector<GDX12Material*> materials)
        : MeshHandler(meshHandle), Materials(materials), _numFramesDirty(0), DirtyFlag(true)
    {
    }
};