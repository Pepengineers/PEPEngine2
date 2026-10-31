#pragma once

#include "Engine.Core/Types/MeshTypes.h"
#include <algorithm>
#include <cctype>
#include <unordered_map>

struct BistroPropGroup
{
    std::string Name;
    std::vector<std::uint32_t> SubMeshIndices;
};

struct BistroPropPartition
{
    std::vector<std::uint32_t> StaticSubMeshes;
    std::vector<BistroPropGroup> Props;
};

inline BistroPropPartition PartitionBistroProps(const Engine::Core::Mesh& mesh)
{
    BistroPropPartition result;
    std::unordered_map<std::string, size_t> groups;
    const auto& parts = mesh.GetSubMeshes();
    for (size_t index = 0; index < parts.size(); ++index)
    {
        const auto& name = parts[index].SourceNodeName;
        std::string lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lower.find("liquorbottle") == std::string::npos && lower.find("chair") == std::string::npos && lower.find("table") == std::string::npos)
        {
            result.StaticSubMeshes.push_back(static_cast<std::uint32_t>(index));
            continue;
        }
        const auto inserted = groups.emplace(name, result.Props.size());
        if (inserted.second) result.Props.push_back({ name, {} });
        result.Props[inserted.first->second].SubMeshIndices.push_back(static_cast<std::uint32_t>(index));
    }
    return result;
}
