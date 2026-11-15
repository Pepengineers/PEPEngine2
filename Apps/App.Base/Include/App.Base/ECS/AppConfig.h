// AppConfig.h
#pragma once

#include <filesystem>
#include <string>
#include <vector>

struct AppConfig
{
    std::string Name;
    std::string Type;
    std::filesystem::path StartScene;
    std::vector<std::string> SystemsBanlist;

    bool IsSystemBanned(const std::string& systemName) const
    {
        for (const std::string& bannedSystem : SystemsBanlist)
        {
            if (bannedSystem == systemName)
            {
                return true;
            }
        }

        return false;
    }
};

struct SceneWorldConfig
{
    std::string Name;
    std::filesystem::path Path;
};

struct SceneConfig
{
    std::string Name;
    std::vector<SceneWorldConfig> Worlds;
};

struct SystemConfig
{
    std::string Name;
    int Priority = 0;
};

struct WorldConfig
{
    std::string Name;
    std::vector<SystemConfig> Systems;
};
