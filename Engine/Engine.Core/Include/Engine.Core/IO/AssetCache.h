#pragma once
#include <filesystem>
#include <fstream>
#include <string>
#include <cstdint>
#include <vector>
#include <windows.h>

namespace Engine::Core
{
    inline std::vector<std::filesystem::path> ObjMaterialLibraries(const std::filesystem::path& source);
    // Derived data only: source size/time, importer version and options determine the cache key.
    inline std::filesystem::path AssetCachePath(const std::filesystem::path& source, const std::string& version,
        bool includeMaterials = false)
    {
        std::error_code error;
        std::uint64_t hash = 14695981039346656037ull;
        const auto append = [&](const std::string& value)
        {
            for (const unsigned char byte : value) { hash ^= byte; hash *= 1099511628211ull; }
        };
        const auto stamp = [&](const std::filesystem::path& path)
        {
            append(path.lexically_normal().u8string());
            append(std::to_string(std::filesystem::file_size(path, error)));
            if (error) return false;
            append(std::to_string(std::filesystem::last_write_time(path, error).time_since_epoch().count()));
            return !error;
        };
        append(version);
        if (!stamp(source)) return {};
        if (includeMaterials)
        {
            for (const auto& material : ObjMaterialLibraries(source))
                if (!stamp(material)) return {};
        }
        const auto folder = std::filesystem::path(SOLUTION_DIR) / "Build" / "AssetCache";
        std::filesystem::create_directories(folder, error);
        if (error) return {};
        return folder / (std::to_string(hash) + "." + version);
    }

    inline std::filesystem::path CacheTemporaryPath(const std::filesystem::path& path)
    {
        return path.wstring() + L"." + std::to_wstring(GetCurrentProcessId()) + L"." +
            std::to_wstring(GetCurrentThreadId()) + L".tmp";
    }

    inline void PublishAssetCache(const std::filesystem::path& temporary, const std::filesystem::path& path)
    {
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
        {
            std::error_code error;
            std::filesystem::remove(temporary, error);
        }
    }

    inline std::vector<std::filesystem::path> ObjMaterialLibraries(const std::filesystem::path& source)
    {
        const auto cache = AssetCachePath(source, "mtllib-v1");
        std::vector<std::filesystem::path> result;
        std::string line;
        if (!cache.empty())
        {
            std::ifstream saved(cache);
            while (std::getline(saved, line))
            {
                if (line == "#end") return result;
                result.push_back(std::filesystem::u8path(line));
            }
            result.clear();
        }
        // Cache the tiny dependency list so warm loads never scan gigabytes of OBJ text.
        std::ifstream input(source);
        while (std::getline(input, line))
        {
            const auto first = line.find_first_not_of(" \t");
            if (first == std::string::npos || line.size() <= first + 6 ||
                _strnicmp(line.c_str() + first, "mtllib", 6) != 0 ||
                (line[first + 6] != ' ' && line[first + 6] != '\t')) continue;
            const auto start = line.find_first_not_of(" \t", first + 6);
            if (start == std::string::npos) continue;
            auto name = line.substr(start);
            const auto comment = name.find('#');
            if (comment != std::string::npos) name.erase(comment);
            name.erase(name.find_last_not_of(" \t\r") + 1);
            if (!name.empty()) result.push_back((source.parent_path() / std::filesystem::u8path(name)).lexically_normal());
        }
        if (!cache.empty() && input.eof())
        {
            const auto temporary = CacheTemporaryPath(cache);
            std::ofstream output(temporary);
            for (const auto& material : result) output << material.u8string() << '\n';
            output << "#end\n";
            output.close();
            if (output) PublishAssetCache(temporary, cache);
        }
        return result;
    }
}
