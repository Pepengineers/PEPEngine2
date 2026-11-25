#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "Engine.Core/ECS/Entity.h"
#include "Engine.Core/AssetHandles.h"

class World;
class RenderModule;
struct GPUTexture;
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

    struct MeshResource
    {
        Engine::Core::MeshHandle Handle;
        const Engine::Core::Mesh* Mesh;
        std::vector<GDX12Material*> ImportedMaterials;
    };
    std::unordered_map<std::string, MeshResource> MeshesById;

    std::unordered_map<std::string, GPUTexture*> TexturesById;
    std::unordered_map<std::string, GDX12Material*> MaterialsById;

    Entity ResolveEntity(const std::string& id) const
    {
        const auto it = EntitiesById.find(id);
        if (it == EntitiesById.end()) { return InvalidEntity; }

        return it->second;
    }

    const MeshResource* ResolveMesh(const std::string& id) const
    {
        const auto it = MeshesById.find(id);
        return it == MeshesById.end() ? nullptr : &it->second;
    }

    GPUTexture* ResolveTexture(const std::string& id) const
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
