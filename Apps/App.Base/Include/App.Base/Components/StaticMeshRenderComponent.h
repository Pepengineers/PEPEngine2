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

    StaticMeshRenderComponent(Engine::Core::MeshHandle meshHandle, std::vector<GDX12Material*> materials)
        : MeshHandler(meshHandle), Materials(materials)
    {
    }
};