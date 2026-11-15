#include "App.Base/ECS/AssetCatalog.h"

#include <Windows.h>
#include <objbase.h>

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

#include "App.Base/ECS/YamlReadUtils.h"
#include "Common/Logger.h"

#pragma comment(lib, "Ole32.lib")

namespace
{
    namespace fs = std::filesystem;
    constexpr auto MetadataTempExtension = ".pepmeta-tmp";

    std::string NormalizeUuid(std::string uuid, const std::filesystem::path& owner)
    {
        const auto invalid = [&]
        {
            return std::runtime_error(owner.u8string() + " has an invalid asset UUID '" + uuid + "'");
        };

        if (uuid.size() != 36 || uuid == "00000000-0000-0000-0000-000000000000")
        {
            throw invalid();
        }

        for (size_t i = 0; i < uuid.size(); ++i)
        {
            char& ch = uuid[i];
            if (i == 8 || i == 13 || i == 18 || i == 23)
            {
                if (ch != '-')
                {
                    throw invalid();
                }
                continue;
            }

            if (ch >= 'A' && ch <= 'F')
            {
                ch += 'a' - 'A';
            }
            if (!(ch >= '0' && ch <= '9') && !(ch >= 'a' && ch <= 'f'))
            {
                throw invalid();
            }
        }
        return uuid;
    }

    void CreateSidecar(const fs::path& sidecar)
    {
        GUID guid;
        wchar_t text[39];
        if (FAILED(CoCreateGuid(&guid)) || StringFromGUID2(guid, text, 39) != 39)
        {
            throw std::runtime_error("Failed to generate asset UUID for " + sidecar.u8string());
        }

        std::string uuid(36, '\0');
        std::transform(text + 1, text + 37, uuid.begin(), [](wchar_t ch) { return static_cast<char>(ch); });
        uuid = NormalizeUuid(std::move(uuid), sidecar);
        const auto temporary = sidecar.parent_path() / (uuid + MetadataTempExtension);
        std::ofstream file(temporary, std::ios::binary);
        file << "Uuid: \"" << uuid << "\"\n";
        file.close();

        if (file && MoveFileExW(temporary.c_str(), sidecar.c_str(), 0))
        {
            return;
        }

        const DWORD error = file ? GetLastError() : ERROR_WRITE_FAULT;
        std::error_code cleanupError;
        fs::remove(temporary, cleanupError);

        // concurrent scan may have published this asset's sidecar first
        if (error != ERROR_ALREADY_EXISTS && error != ERROR_FILE_EXISTS)
        {
            throw fs::filesystem_error("Failed to create asset metadata", sidecar, {static_cast<int>(error), std::system_category()});
        }
    }
}

AssetCatalog::AssetCatalog(const std::filesystem::path& projectRoot)
{
    const auto registerAsset = [this](const fs::path& asset, const fs::path& sidecar)
    {
        const ryml::Tree tree = AppYaml::LoadFile(sidecar);
        const auto meta = tree.rootref();
        if (!meta.is_map() || !meta.has_child("Uuid") || !meta["Uuid"].has_val())
        {
            throw std::runtime_error(sidecar.u8string() + " requires a scalar Uuid");
        }

        std::string uuid;
        meta["Uuid"] >> uuid;
        uuid = NormalizeUuid(std::move(uuid), sidecar);
        const auto [existing, inserted] = _paths.emplace(uuid, asset);
        if (!inserted)
        {
            throw std::runtime_error("Duplicate asset UUID " + uuid + " in " + existing->second.u8string() + ".meta and " + sidecar.u8string());
        }
    };

    std::vector<fs::path> missingMetadata;
    const auto root = fs::absolute(projectRoot).lexically_normal();
    for (const auto* directory : {"Assets", "Scenes", "Worlds"})
    {
        const auto assetRoot = root / directory;
        if (!fs::exists(assetRoot))
        {
            continue;
        }

        for (const auto& entry : fs::recursive_directory_iterator(assetRoot))
        {
            if (!entry.is_regular_file())
            {
                continue;
            }

            const auto& path = entry.path();
            const auto extension = path.extension();
            if (extension == MetadataTempExtension)
            {
                continue;
            }
            if (_wcsicmp(extension.c_str(), L".meta") == 0)
            {
                auto asset = path;
                asset.replace_extension();
                if (!fs::is_regular_file(asset))
                {
                    registerAsset(asset, path);
                }
                continue;
            }

            auto sidecar = path;
            sidecar += ".meta";
            if (fs::exists(sidecar))
            {
                registerAsset(path, sidecar);
            }
            else
            {
                missingMetadata.push_back(path);
            }
        }
    }

    for (const auto& asset : missingMetadata)
    {
        auto sidecar = asset;
        sidecar += ".meta";
        if (!fs::exists(sidecar))
        {
            try
            {
                CreateSidecar(sidecar);
            }
            catch (const fs::filesystem_error& error)
            {
                Logger::Warn("Could not create asset metadata '{}': {}", sidecar.u8string(), error.code().message());
                continue;
            }
        }
        registerAsset(asset, sidecar);
    }
}

const AssetCatalog& AssetCatalog::Project()
{
    // todo: rebuild the catalog on asset changes when live editing is added.
    static const AssetCatalog catalog(std::filesystem::current_path());
    return catalog;
}

std::filesystem::path AssetCatalog::Find(const std::string& uuid, const std::filesystem::path& owner) const
{
    const auto it = _paths.find(NormalizeUuid(uuid, owner));
    if (it == _paths.end() || !std::filesystem::is_regular_file(it->second))
    {
        return {};
    }
    return it->second;
}

std::filesystem::path AppYaml::ResolveAssetReference(
    const ryml::NodeRef& node, const char* assetKey, const char* legacyKey,
    const std::filesystem::path& owner, const AssetCatalog* catalog, bool required)
{
    if (!node.is_map())
    {
        throw std::runtime_error(owner.u8string() + " requires an asset reference map");
    }

    const bool hasAsset = HasChild(node, assetKey);
    if (hasAsset == HasChild(node, legacyKey))
    {
        throw std::runtime_error(owner.u8string() + " requires exactly one of " + assetKey + " or " + legacyKey);
    }

    const char* key = hasAsset ? assetKey : legacyKey;
    if (!node[ryml::to_csubstr(key)].has_val())
    {
        throw std::runtime_error(owner.u8string() + " requires a scalar " + key);
    }

    const auto value = ReadString(node, key);
    if (value.empty())
    {
        throw std::runtime_error(owner.u8string() + " has an empty " + key);
    }
    if (!hasAsset)
    {
        return std::filesystem::absolute(std::filesystem::u8path(value)).lexically_normal();
    }

    auto path = (catalog ? *catalog : AssetCatalog::Project()).Find(value, owner);
    if (!path.empty())
    {
        return path;
    }

    const auto message = owner.u8string() + " references missing asset " + value + " (check the asset and its .meta file)";
    if (required)
    {
        throw std::runtime_error(message);
    }
    Logger::Warn("{}", message);
    return {};
}
