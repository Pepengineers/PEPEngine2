#pragma once

#include <string>
#include <vector>

struct TextureResourceConfig
{
    std::string Id;
    std::string Source;
};

struct MaterialResourceConfig
{
    std::string Id;
    float Metallic = 0.0f;
    float Roughness = 1.0f;
    std::string Diffuse;
    std::string Normal;
    std::string Specular;
    std::string RoughnessMap;
    std::string Emissive;
};

struct MeshResourceConfig
{
    std::string Id;
    std::string Source;
    bool SubmitToRenderer = true;
    bool ImportMaterials = false;
};

struct WorldResourcesConfig
{
    std::vector<TextureResourceConfig> Textures;
    std::vector<MaterialResourceConfig> Materials;
    std::vector<MeshResourceConfig> Meshes;
};

struct WorldEntityConfig
{
    std::string Id;
    // todo ryml::NodeRef/id on Components node and parse to WorldLoader.
};

struct WorldFileConfig
{
    std::string Name;
    WorldResourcesConfig Resources;
    std::string ActiveCamera;
};
