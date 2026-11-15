#include "App.Base/ECS/WorldDocument.h"

#include <stdexcept>

#include "App.Base/ECS/AssetCatalog.h"
#include "App.Base/ECS/YamlReadUtils.h"

namespace
{
    WorldTextureResource ParseTextureResource(ryml::NodeRef textureNode,
        const std::filesystem::path& owner, const AssetCatalog* catalog)
    {
        WorldTextureResource resource;
        resource.Id = AppYaml::ReadString(textureNode, "Id");
        if (resource.Id.empty())
        {
            throw std::runtime_error(owner.u8string() + " has a texture resource without an Id");
        }

        if (textureNode.has_child("Required"))
        {
            textureNode["Required"] >> resource.Required;
        }
        resource.SourcePath = AppYaml::ResolveAssetReference(textureNode, "Asset", "Source", owner, catalog, resource.Required);
        return resource;
    }

    WorldMeshResource ParseMeshResource(ryml::NodeRef meshNode, const std::filesystem::path& owner, const AssetCatalog* catalog)
    {
        WorldMeshResource resource;
        resource.Id = AppYaml::ReadString(meshNode, "Id");
        if (resource.Id.empty())
        {
            throw std::runtime_error(owner.u8string() + " has a mesh resource without an Id");
        }

        if (meshNode.has_child("SubmitToRenderer"))
        {
            meshNode["SubmitToRenderer"] >> resource.SubmitToRenderer;
        }

        if (meshNode.has_child("ImportMaterials"))
        {
            meshNode["ImportMaterials"] >> resource.ImportMaterials;
        }

        resource.SourcePath = AppYaml::ResolveAssetReference(meshNode, "Asset", "Source", owner, catalog);
        return resource;
    }
}

WorldDocument ParseWorldDocument(const std::filesystem::path& path, const AssetCatalog* catalog)
{
    if (path.empty())
    {
        throw std::runtime_error("World path is empty");
    }
    
    WorldDocument document;
    document.SourcePath = std::filesystem::absolute(path).lexically_normal();
    document.YamlTree = AppYaml::LoadFile(document.SourcePath);

    ryml::NodeRef root = document.YamlTree.rootref();
    if (!root.is_map() || !root.has_child("World") || !root["World"].is_map())
    {
        throw std::runtime_error("World YAML requires a 'World' map: " + document.SourcePath.u8string());
    }
    
    ryml::NodeRef worldNode = root["World"];
    
    document.Config.Name = AppYaml::ReadString(worldNode, "Name");
    
    if (worldNode.has_child("Systems"))
    {
        for (ryml::NodeRef systemNode : worldNode["Systems"].children())
        {
            SystemConfig systemConfig;
            systemConfig.Name = AppYaml::ReadString(systemNode, "Name");
            systemConfig.Priority = AppYaml::ReadInt(systemNode, "Priority");
            
            if (!systemConfig.Name.empty())
            {
                document.Config.Systems.push_back(std::move(systemConfig));
            }
        }
    }
    
    if (worldNode.has_child("Resources"))
    {
        ryml::NodeRef resourcesNode = worldNode["Resources"];
        
        if (resourcesNode.has_child("Textures"))
        {
            for (ryml::NodeRef textureNode : resourcesNode["Textures"].children())
            {
                auto resource = ParseTextureResource(textureNode, document.SourcePath, catalog);
                if (!resource.SourcePath.empty())
                {
                    document.Textures.push_back(std::move(resource));
                }
            }
        }
        
        if (resourcesNode.has_child("Meshes"))
        {
            for (ryml::NodeRef meshNode : resourcesNode["Meshes"].children())
            {
                document.Meshes.push_back(ParseMeshResource(meshNode, document.SourcePath, catalog));
            }
        }
    }

    return document;
}
