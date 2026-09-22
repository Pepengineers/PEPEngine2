#pragma once
#include <filesystem>
#include <memory>
#include <vector>
#include <string>

#include "AppConfig.h"
#include "WorldDocument.h"
#include "Engine.Core/Types/TextureTypes.h"
#include "Engine.Core/Types/MeshTypes.h"

struct PreparedMesh
{
    WorldMeshResource Resource;
    std::unique_ptr<Engine::Core::Mesh> Data;
};

struct PreparedTexture
{
    WorldTextureResource Resource;
    std::unique_ptr<Engine::Core::Texture> Data;
};

struct PreparedWorld
{
    SceneWorldConfig Config;
    WorldDocument Document;
    
    std::vector<PreparedMesh> Meshes;
    std::vector<PreparedTexture> Textures;
};

struct PreparedScene
{
    std::string SourcePath;
    SceneConfig Config;
    std::vector<PreparedWorld> Worlds;
};