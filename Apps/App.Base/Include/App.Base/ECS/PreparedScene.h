#pragma once
#include <filesystem>
#include <memory>
#include <vector>
#include <string>

#include "AppConfig.h"
#include "Engine.Core/Types/TextureTypes.h"
#include "Engine.Core/Types/MeshTypes.h"

struct PreparedMesh
{
    std::string Id;
    std::filesystem::path SourcePath;
    bool SubmitToRenderer = true;
    bool ImportMaterials = false;
    std::unique_ptr<Engine::Core::Mesh> Data;
};

struct PreparedTexture
{
    std::filesystem::path SourcePath;
    bool Required = false;
    std::unique_ptr<Engine::Core::Texture> Data;
};

struct PreparedWorld
{
    SceneWorldConfig Config;
    
    std::string YamlText;
    
    std::vector<PreparedMesh> Meshes;
    std::vector<PreparedTexture> Textures;
};

struct PreparedScene
{
    std::string SourcePath;
    SceneConfig Config;
    std::vector<PreparedWorld> Worlds;
};