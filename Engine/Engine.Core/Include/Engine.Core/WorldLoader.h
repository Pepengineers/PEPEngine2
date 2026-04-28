#pragma once
#include <filesystem>

class World;

class WorldLoader
{
public:

    static bool LoadFromFile(World& world, const std::filesystem::path& path);

    static bool SaveToFile(World& world, const std::filesystem::path& path);
};