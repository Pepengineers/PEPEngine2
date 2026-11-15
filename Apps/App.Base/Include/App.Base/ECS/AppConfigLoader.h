// AppConfigLoader.h
#pragma once

#include "App.Base/ECS/AppConfig.h"

class AssetCatalog;

class AppConfigLoader
{
public:
    static AppConfig LoadAppConfig(const std::filesystem::path& path, const AssetCatalog* catalog = nullptr);
    static SceneConfig LoadSceneConfig(const std::filesystem::path& path, const AssetCatalog* catalog = nullptr);
    static WorldConfig LoadWorldConfig(const std::filesystem::path& path);
};
