// SceneImporter.h

#pragma once

#include <filesystem>

#include <Engine.Core/Types/SceneTypes.h>

namespace Engine::Core
{
	class MeshRegistry;

	class SceneImporter
	{
	public:
		[[nodiscard]] static std::shared_ptr<SceneAsset> ImportSceneAsset(const std::filesystem::path& sourcePath, MeshRegistry& meshRegistry);
	};
}