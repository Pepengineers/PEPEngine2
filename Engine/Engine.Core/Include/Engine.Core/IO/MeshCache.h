#pragma once
#include "Engine.Core/Types/MeshTypes.h"
#include <functional>
#include <memory>

namespace Engine::Core
{
    // Increment when the stored fields or their interpretation change.
    inline constexpr unsigned MeshCacheVersion = 1;
    std::unique_ptr<Mesh> LoadMeshCache(const std::filesystem::path& path, const std::function<bool()>& keepLoading = {});
    bool SaveMeshCache(const std::filesystem::path& path, const Mesh& mesh, const std::function<bool()>& keepLoading = {});
}
