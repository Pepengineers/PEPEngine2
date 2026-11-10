#pragma once
#include <filesystem>
#include <functional>
#include <vector>
#include <string>
namespace Engine::Core { class Mesh; }
class GDX12Material;
class RenderModule;

class World;

class WorldLoader
{
public:

    static std::vector<GDX12Material*> BuildMaterialSlots(RenderModule& render,
        const Engine::Core::Mesh& mesh, const std::string& resourceName);

    using ObjSceneReplay = std::function<void(World&)>;
    static bool LoadFromFile(World& world, const std::filesystem::path& path, ObjSceneReplay* replay = nullptr);

    static bool SaveToFile(World& world, const std::filesystem::path& path);
};
