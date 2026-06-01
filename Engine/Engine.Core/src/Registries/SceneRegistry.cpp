// SceneRegistry.cpp

#include <Engine.Core/Registries/SceneRegistry.h>
#include <Engine.Core/Registries/MeshRegistry.h>
#include <Engine.Core/IO/SceneImporter.h>

namespace Engine::Core
{
	const wchar_t* SceneRegistry::GetRegistryName() const
	{
		return L"SceneRegistry";
	}

	std::filesystem::path SceneRegistry::ResolveSourcePath(const std::filesystem::path& path) const
	{
		if (path.is_absolute())
		{
			return path.lexically_normal();
		}

		std::filesystem::path resolvedPath = MODELS_FOLDER;
		resolvedPath /= path;
		return resolvedPath.lexically_normal();
	}

	std::filesystem::path SceneRegistry::MakeMetadataForRegisteredPath(const std::filesystem::path& resolvedSourcePath) const
	{
		return resolvedSourcePath;
	}

	std::filesystem::path SceneRegistry::GetSourcePathFromMetadata(const std::filesystem::path& metadata) const
	{
		return metadata;
	}

	std::wstring SceneRegistry::GetCacheKeyFromMetadata(const std::filesystem::path& metadata) const
	{
		return BuildCacheKeyFromResolvedPath(metadata);
	}

	const wchar_t* SceneRegistry::GetAssetTypeName() const
	{
		return L"scene";
	}

	const SceneAsset* SceneRegistry::GetScene(const SceneHandle handle) const
	{
		return GetAsset(handle);
	}

	const SceneAsset* SceneRegistry::FindScene(const std::filesystem::path& path) const
	{
		return GetScene(FindHandle(path));
	}

	const SceneAsset* SceneRegistry::Load(const std::filesystem::path& path, MeshRegistry& meshRegistry)
	{
		if (path.empty())
		{
			WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Failed to load scene: empty path.\n");
			return nullptr;
		}

		const SceneAsset* cachedScene = FindScene(path);
		if (cachedScene != nullptr)
		{
			WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Reusing cached scene for path: " +
				ResolveSourcePath(path).generic_wstring() + L"\n");

			return cachedScene;
		}

		const SceneHandle sceneHandle = Register(path);
		if (!sceneHandle.IsValid())
		{
			return nullptr;
		}

		const std::filesystem::path sourcePath = GetSourcePath(sceneHandle);
		if (sourcePath.empty())
		{
			WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Failed to load scene: could not resolve source path.\n");
			return nullptr;
		}

		WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Importing scene from path: " +
			sourcePath.generic_wstring() + L"\n");

		std::unique_ptr<SceneAsset> importedScene = SceneImporter::ImportSceneAsset(sourcePath, meshRegistry);
		if (importedScene == nullptr)
		{
			WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Failed to import scene from path: " +
				sourcePath.generic_wstring() + L"\n");
			return nullptr;
		}

		WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Imported scene with " +
			std::to_wstring(importedScene->GetNodeCount()) + L" nodes from path: " +
			sourcePath.generic_wstring() + L"\n");

		return Cache(sceneHandle, std::move(importedScene));
	}
}