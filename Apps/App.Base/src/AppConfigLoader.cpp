#include "App.Base/AppConfigLoader.h"

#include <ryml.hpp>
#include <ryml_std.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

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

    bool HasChild(const ryml::NodeRef& node, const char* name)
    {
        return node.has_child(ryml::to_csubstr(name));
    }

    std::string ReadString(const ryml::NodeRef& node, const char* name, const std::string& defaultValue = "")
    {
        if (!HasChild(node, name))
        {
            return defaultValue;
        }

        std::string value;
        node[ryml::to_csubstr(name)] >> value;
        return value;
    }

    int ReadInt(const ryml::NodeRef& node, const char* name, int defaultValue = 0)
    {
        if (!HasChild(node, name))
        {
            return defaultValue;
        }

        int value = defaultValue;
        node[ryml::to_csubstr(name)] >> value;
        return value;
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

    if (!HasChild(root, "App"))
    {
        throw std::runtime_error("App config does not contain root node 'App': " + path);
    }

    ryml::NodeRef appNode = root["App"];

    AppConfig config;
    config.Name = ReadString(appNode, "Name");
    config.Type = ReadString(appNode, "Type");
    config.StartScene = ReadString(appNode, "StartScene");

    if (HasChild(appNode, "SystemsBanlist"))
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

    if (!HasChild(root, "Scene"))
    {
        throw std::runtime_error("Scene config does not contain root node 'Scene': " + path);
    }

    ryml::NodeRef sceneNode = root["Scene"];

    SceneConfig config;
    config.Name = ReadString(sceneNode, "Name");

    if (HasChild(sceneNode, "Worlds"))
    {
        ryml::NodeRef worldsNode = sceneNode["Worlds"];

        for (ryml::NodeRef worldNode : worldsNode.children())
        {
            SceneWorldConfig worldConfig;
            worldConfig.Name = ReadString(worldNode, "Name");
            worldConfig.Path = ReadString(worldNode, "Path");

            config.Worlds.push_back(worldConfig);
        }
    }

    return config;
}

WorldConfig AppConfigLoader::LoadWorldConfig(const std::string& path)
{
    ryml::Tree tree = ParseYamlTree(path);
    ryml::NodeRef root = tree.rootref();

    if (!HasChild(root, "World"))
    {
        throw std::runtime_error("World config does not contain root node 'World': " + path);
    }

    ryml::NodeRef worldNode = root["World"];

    WorldConfig config;
    config.Name = ReadString(worldNode, "Name");
    config.SourcePath = ReadString(worldNode, "SourcePath");

    if (HasChild(worldNode, "Systems"))
    {
        ryml::NodeRef systemsNode = worldNode["Systems"];

        for (ryml::NodeRef systemNode : systemsNode.children())
        {
            SystemConfig systemConfig;
            systemConfig.Name = ReadString(systemNode, "Name");
            systemConfig.Priority = ReadInt(systemNode, "Priority", 0);

            config.Systems.push_back(systemConfig);
        }
    }

    return config;
}
