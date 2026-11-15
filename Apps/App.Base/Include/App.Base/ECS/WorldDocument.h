#pragma once
#include <filesystem>
#include <ryml.hpp>

#include "AppConfig.h"

class AssetCatalog;

struct WorldTextureResource
{
    std::string Id;
    std::filesystem::path SourcePath;
    bool Required = true;
};

struct WorldMeshResource
{
    std::string Id;
    std::filesystem::path SourcePath;
    bool SubmitToRenderer = true;
    bool ImportMaterials = false;
};

struct WorldDocument
{
    WorldDocument() = default;
    WorldDocument(WorldDocument&&) noexcept = default;
    WorldDocument& operator=(WorldDocument&&) noexcept = default;
    
    WorldDocument(const WorldDocument&) = delete;
    WorldDocument& operator=(const WorldDocument&) = delete;
    
    std::filesystem::path SourcePath;
    ryml::Tree YamlTree;
    
    WorldConfig Config;
    std::vector<WorldMeshResource> Meshes;
    std::vector<WorldTextureResource> Textures;
    
    [[nodiscard]] ryml::NodeRef GetWorldNode()
    {
        return YamlTree.rootref()["World"];
    }
};

[[nodiscard]] WorldDocument ParseWorldDocument(const std::filesystem::path& path, const AssetCatalog* catalog = nullptr);
