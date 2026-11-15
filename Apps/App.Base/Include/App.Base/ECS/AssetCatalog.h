#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>

#include <ryml.hpp>

class AssetCatalog
{
public:
    explicit AssetCatalog(const std::filesystem::path& projectRoot);
    static const AssetCatalog& Project();

    // returns an empty path for a valid uuid whose asset is missing.
    std::filesystem::path Find(const std::string& uuid, const std::filesystem::path& owner) const;

private:
    std::unordered_map<std::string, std::filesystem::path> _paths;
};

namespace AppYaml
{
    std::filesystem::path ResolveAssetReference(
        const ryml::NodeRef& node, const char* assetKey, const char* legacyKey,
        const std::filesystem::path& owner, const AssetCatalog* catalog, bool required = true);
}
