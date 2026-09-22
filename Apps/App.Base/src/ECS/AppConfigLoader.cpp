#include "App.Base/ECS/AppConfigLoader.h"

#include <ryml.hpp>
#include <ryml_std.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include "App.Base/ECS/WorldDocument.h"
#include "App.Base/ECS/YamlReadUtils.h"

namespace
{
    std::string ReadTextFile(const std::string& path)
    {
        std::ifstream file(path, std::ios::binary);

        if (!file.is_open())
        {
            throw std::runtime_error(
                "Failed to open yaml file: " + path +
                "\nCurrent working directory: " + std::filesystem::current_path().string());
        }

        std::stringstream buffer;
        buffer << file.rdbuf();

        return buffer.str();
    }

    ryml::Tree ParseYamlTree(const std::string& path)
    {
        const std::string yamlText = ReadTextFile(path);

        try
        {
            return ryml::parse_in_arena(
                ryml::to_csubstr(path),
                ryml::to_csubstr(yamlText));
        }
        catch (const std::exception& exception)
        {
            throw std::runtime_error(
                "Failed to parse yaml file: " + path +
                "\nReason: " + exception.what());
        }
    }
}

AppConfig AppConfigLoader::LoadAppConfig(const std::string& path)
{
    ryml::Tree tree = ParseYamlTree(path);
    ryml::NodeRef root = tree.rootref();

    if (!AppYaml::HasChild(root, "App"))
    {
        throw std::runtime_error("App config does not contain root node 'App': " + path);
    }

    ryml::NodeRef appNode = root["App"];

    AppConfig config;
    config.Name = AppYaml::ReadString(appNode, "Name");
    config.Type = AppYaml::ReadString(appNode, "Type");
    config.StartScene = AppYaml::ReadString(appNode, "StartScene");

    if (AppYaml::HasChild(appNode, "SystemsBanlist"))
    {
        ryml::NodeRef banlistNode = appNode["SystemsBanlist"];

        for (ryml::NodeRef item : banlistNode.children())
        {
            std::string systemName;
            item >> systemName;
            config.SystemsBanlist.push_back(systemName);
        }
    }

    return config;
}

SceneConfig AppConfigLoader::LoadSceneConfig(const std::string& path)
{
    ryml::Tree tree = ParseYamlTree(path);
    ryml::NodeRef root = tree.rootref();

    if (!AppYaml::HasChild(root, "Scene"))
    {
        throw std::runtime_error("Scene config does not contain root node 'Scene': " + path);
    }

    ryml::NodeRef sceneNode = root["Scene"];

    SceneConfig config;
    config.Name = AppYaml::ReadString(sceneNode, "Name");

    if (AppYaml::HasChild(sceneNode, "Worlds"))
    {
        ryml::NodeRef worldsNode = sceneNode["Worlds"];

        for (ryml::NodeRef worldNode : worldsNode.children())
        {
            SceneWorldConfig worldConfig;
            worldConfig.Name = AppYaml::ReadString(worldNode, "Name");
            const std::filesystem::path sceneDirectory = std::filesystem::absolute(path).parent_path();

            std::filesystem::path worldPath = AppYaml::ReadString(worldNode, "Path");
            if (worldPath.is_relative())
            {
                worldPath = sceneDirectory / worldPath;
            }

            worldConfig.Path = worldPath.lexically_normal().string();

            config.Worlds.push_back(worldConfig);
        }
    }

    return config;
}

WorldConfig AppConfigLoader::LoadWorldConfig(const std::string& path)
{
    WorldDocument document = ParseWorldDocument(path);
    return std::move(document.Config);
}
