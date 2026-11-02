#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "Engine.Core/ECS/Entity.h"
#include "Engine.Core/AssetHandles.h"

class World;
class RenderModule;
class GDX12Texture;
class GDX12Material;

namespace Engine::Core
{
    class AssetManager;
    class Mesh;
}

struct WorldLoadContext
{
    World* World = nullptr;
    RenderModule* Render = nullptr;
    Engine::Core::AssetManager* AssetManager = nullptr;

    std::filesystem::path WorldFilePath;
    std::filesystem::path WorldDirectory;

    std::unordered_map<std::string, Entity> EntitiesById;

    std::unordered_map<std::string, Engine::Core::MeshHandle> MeshesById;
    std::unordered_map<std::string, const Engine::Core::Mesh*> CpuMeshesById;
    std::unordered_map<std::string, std::vector<GDX12Material*>> MeshMaterialsById;

    std::unordered_map<std::string, GDX12Texture*> TexturesById;
    std::unordered_map<std::string, GDX12Material*> MaterialsById;
    std::unordered_map<std::wstring, GDX12Texture*> ImportedTexturesByPath;

    Entity ResolveEntity(const std::string& id) const
    {
        const auto it = EntitiesById.find(id);
        if (it == EntitiesById.end()) { return InvalidEntity; }

        return it->second;
    }

    Engine::Core::MeshHandle ResolveMesh(const std::string& id) const
    {
        const auto it = MeshesById.find(id);
        if (it == MeshesById.end()) { return {}; }

        return it->second;
    }

    const Engine::Core::Mesh* ResolveCpuMesh(const std::string& id) const
    {
        const auto it = CpuMeshesById.find(id);
        if (it == CpuMeshesById.end()) { return nullptr; }

        return it->second;
    }

    const std::vector<GDX12Material*>* ResolveMeshMaterials(const std::string& id) const
    {
        const auto it = MeshMaterialsById.find(id);
        if (it == MeshMaterialsById.end()) { return nullptr; }

        return &it->second;
    }

    GDX12Texture* ResolveTexture(const std::string& id) const
    {
        const auto it = TexturesById.find(id);
        if (it == TexturesById.end()) { return nullptr; }

        return it->second;
    }

    GDX12Material* ResolveMaterial(const std::string& id) const
    {
        const auto it = MaterialsById.find(id);
        if (it == MaterialsById.end()) { return nullptr; }

        return it->second;
    }
};
