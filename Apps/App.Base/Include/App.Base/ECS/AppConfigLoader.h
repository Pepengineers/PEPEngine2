// AppConfigLoader.h
#pragma once

#include "App.Base/ECS/AppConfig.h"

#include <string>

class AppConfigLoader
{
public:
    static AppConfig LoadAppConfig(const std::string& path);
    static SceneConfig LoadSceneConfig(const std::string& path);
    static WorldConfig LoadWorldConfig(const std::string& path);
};