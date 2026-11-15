#include "App.Base/ECS/AppConfigLoader.h"

#include <fstream>
#include <sstream>
#include <stdexcept>

#include "App.Base/ECS/AssetCatalog.h"
#include "App.Base/ECS/WorldDocument.h"
#include "App.Base/ECS/YamlReadUtils.h"

ryml::Tree AppYaml::LoadFile(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    const auto fileName = path.u8string();
    if (!file)
    {
        throw std::runtime_error("Failed to open YAML file: " + fileName);
    }

    std::ostringstream buffer;
    buffer << file.rdbuf();
    const auto yaml = buffer.str();

    auto callbacks = ryml::get_callbacks();
    callbacks.set_error_parse([](ryml::csubstr message, const ryml::ErrorDataParse&, void*)
    {
        throw std::runtime_error(std::string(message.str, message.len));
    });
    ryml::Tree tree(callbacks);
    try
    {
        ryml::parse_in_arena(ryml::to_csubstr(fileName), ryml::to_csubstr(yaml), &tree);
    }
    catch (const std::exception& exception)
    {
        throw std::runtime_error("Failed to parse YAML file " + fileName + ": " + exception.what());
    }
    return tree;
}

AppConfig AppConfigLoader::LoadAppConfig(const std::filesystem::path& path, const AssetCatalog* catalog)
{
    ryml::Tree tree = AppYaml::LoadFile(path);
    ryml::NodeRef root = tree.rootref();

    if (!root.is_map() || !root.has_child("App") || !root["App"].is_map())
    {
        throw std::runtime_error("App config requires an 'App' map: " + path.u8string());
    }

    ryml::NodeRef appNode = root["App"];

    AppConfig config;
    config.Name = AppYaml::ReadString(appNode, "Name");
    config.Type = AppYaml::ReadString(appNode, "Type");
    if (!catalog)
    {
        catalog = &AssetCatalog::Project();
    }
    config.StartScene = AppYaml::ResolveAssetReference(appNode, "StartSceneAsset", "StartScene", path, catalog);

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

SceneConfig AppConfigLoader::LoadSceneConfig(const std::filesystem::path& path, const AssetCatalog* catalog)
{
    ryml::Tree tree = AppYaml::LoadFile(path);
    ryml::NodeRef root = tree.rootref();

    if (!root.is_map() || !root.has_child("Scene") || !root["Scene"].is_map())
    {
        throw std::runtime_error("Scene config requires a 'Scene' map: " + path.u8string());
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

            worldConfig.Path = AppYaml::ResolveAssetReference(worldNode, "Asset", "Path", path, catalog);

            config.Worlds.push_back(worldConfig);
        }
    }

    return config;
}

WorldConfig AppConfigLoader::LoadWorldConfig(const std::filesystem::path& path)
{
    WorldDocument document = ParseWorldDocument(path);
    return std::move(document.Config);
}
