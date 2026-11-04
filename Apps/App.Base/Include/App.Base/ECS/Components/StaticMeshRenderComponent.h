#pragma once

#include "Engine.Core/ECS/Component.h"
#include "Engine.Core/AssetHandles.h"
#include "DirectXCollision.h"
#include <cstdint>
#include <vector>
#include <utility>

class GDX12Material;

struct StaticMeshRenderComponent : ComponentTag
{
    Engine::Core::MeshHandle MeshHandler;
    std::vector<GDX12Material*> Materials;
    std::vector<std::uint32_t> SubMeshIndices;
    DirectX::BoundingBox Bounds;

    bool DirtyFlag;
    bool Visible = true;
    bool ReflectionDynamic = false;
    bool _reflectionDynamic = false;

    //we are creating an instance buffer for each submesh
    std::vector<std::uint32_t> _CBufferIndices;
    std::uint32_t _numFramesDirty;

    StaticMeshRenderComponent(Engine::Core::MeshHandle meshHandle, std::vector<GDX12Material*> materials,
        std::vector<std::uint32_t> subMeshIndices = {})
        : MeshHandler(meshHandle), Materials(std::move(materials)), SubMeshIndices(std::move(subMeshIndices)),
          _numFramesDirty(0), DirtyFlag(true)
    {
    }

    size_t GetSubMeshCount(size_t totalCount) const
    {
        return SubMeshIndices.empty() ? totalCount : SubMeshIndices.size();
    }

    std::uint32_t GetSubMeshIndex(size_t localIndex) const
    {
        return SubMeshIndices.empty() ? static_cast<std::uint32_t>(localIndex) : SubMeshIndices.at(localIndex);
    }
};
