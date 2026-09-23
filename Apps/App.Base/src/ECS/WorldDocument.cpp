#include "App.Base/ECS/WorldDocument.h"

#include <fstream>
#include <sstream>

#include "App.Base/ECS/YamlReadUtils.h"

namespace 
{
    std::string ReadTextFile(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open())
        {
            throw std::runtime_error("Failed to open world file: " + path.string());
        }

        std::stringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }
    
    std::filesystem::path ResolveResourcePath(const std::filesystem::path& sourcePath)
    {
        if (sourcePath.empty())
        {
            return {};
        }
        
        return std::filesystem::absolute(sourcePath).lexically_normal();
    }
}


WorldTextureResource ParseTextureResource(ryml::NodeRef textureNode)
{
    if (!textureNode.has_child("Id") || !textureNode.has_child("Source"))
    {
        throw std::runtime_error("World texture resource requires Id and Source");
    }
    
    WorldTextureResource resource;
    
    std::string source;
    textureNode["Id"] >> resource.Id;
    textureNode["Source"] >> source;
    
    if (resource.Id.empty() || source.empty())
    {
        throw std::runtime_error("World texture resource contains an empty Id or Source");
    }
    
    if (textureNode.has_child("Required"))
    {
        textureNode["Required"] >> resource.Required;
    }
    
    resource.SourcePath = ResolveResourcePath(source);
    
    return resource;
}

WorldMeshResource ParseMeshResource(ryml::NodeRef meshNode)
{
    if (!meshNode.has_child("Id") || !meshNode.has_child("Source"))
    {
        throw std::runtime_error("World mesh resource requires Id and Source");
    }
    
    WorldMeshResource resource;
    
    std::string source;
    meshNode["Id"] >> resource.Id;
    meshNode["Source"] >> source;
    
    if (resource.Id.empty() || source.empty())
    {
        throw std::runtime_error("World mesh resource contains an empty Id or Source");
    }
    
    if (meshNode.has_child("SubmitToRenderer"))
    {
        meshNode["SubmitToRenderer"] >> resource.SubmitToRenderer;
    }
    
    if (meshNode.has_child("ImportMaterials"))
    {
        meshNode["ImportMaterials"] >> resource.ImportMaterials;
    }
    
    resource.SourcePath = ResolveResourcePath(source);
    
    return resource;
}

WorldDocument ParseWorldDocument(const std::filesystem::path& path)
{
    if (path.empty())
    {
        throw std::runtime_error("World path is empty");
    }
    
    WorldDocument document;
    document.SourcePath = std::filesystem::absolute(path).lexically_normal();
    document.YamlText = ReadTextFile(document.SourcePath);

    const std::string pathText = document.SourcePath.string();

    try
    {
        document.YamlTree = ryml::parse_in_arena(ryml::to_csubstr(pathText), ryml::to_csubstr(document.YamlText));
    }
    catch (const std::exception& exception)
    {
        throw std::runtime_error("Failed to parse world yaml '" + document.SourcePath.string() + "': " + exception.what());
    }

    ryml::NodeRef root = document.YamlTree.rootref();
    if (!root.has_child("World"))
    {
        throw std::runtime_error("World yaml does not contain root node 'World': " + document.SourcePath.string());
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
                document.Textures.push_back(ParseTextureResource(textureNode));
            }
        }
        
        if (resourcesNode.has_child("Meshes"))
        {
            for (ryml::NodeRef meshNode : resourcesNode["Meshes"].children())
            {
                document.Meshes.push_back(ParseMeshResource(meshNode));
            }
        }
    }

    return document;
}
