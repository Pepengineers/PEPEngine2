// AssetManager.cpp

#include <Engine.Core/AssetManager.h>

#include <cassert>
#include <cwctype>

namespace Engine::Core
{
#pragma region Internal Helpers
	std::wstring AssetManager::BuildMeshCacheKey(const std::filesystem::path& meshPath)
	{
		// lexically_normal - normalizes the path without accessing the file system (removes '.', '..' and extra slashes)
		// (e.g. "models/../models/./cube.fbx" -> "models/cube.fbx")
		const std::filesystem::path normalizedPath = meshPath.lexically_normal();
		// generic_wstring - converts the path to a string with a unified slash format (with '/' instead of '\')
		// (e.g. "models\\cube.fbx" -> "models/cube.fbx")
		std::wstring cacheKey = normalizedPath.generic_wstring();

		std::transform(cacheKey.begin(), cacheKey.end(), cacheKey.begin(), [](const wchar_t character) { return static_cast<wchar_t>(std::towlower(character)); });
		return cacheKey;
	}
#pragma endregion Internal Helpers

#pragma region Singleton
	// Meyers Singleton
	AssetManager& AssetManager::GetInstance()
	{
		static AssetManager instance;
		return instance;
	}
#pragma endregion Singleton

#pragma region Mesh Cache
	bool AssetManager::IsMeshLoaded(const std::filesystem::path& meshPath) const
	{
		if (meshPath.empty())
		{
			return false;
		}

		return _meshAssets.find(BuildMeshCacheKey(meshPath)) != _meshAssets.end();
	}

	std::shared_ptr<const Mesh> AssetManager::FindMesh(const std::filesystem::path& meshPath) const
	{
		if (meshPath.empty())
		{
			return nullptr;
		}

		const auto meshAssetIterator = _meshAssets.find(BuildMeshCacheKey(meshPath));
		if (meshAssetIterator == _meshAssets.end())
		{
			return nullptr;
		}

		return meshAssetIterator->second.MeshData;
	}

	std::shared_ptr<const Mesh> AssetManager::CacheMesh(const std::filesystem::path& meshPath, std::shared_ptr<Mesh> meshData)
	{
		assert(!meshPath.empty());
		assert(meshData != nullptr);

		if (meshPath.empty() || meshData == nullptr)
		{
			return nullptr;
		}

		const std::wstring cacheKey = BuildMeshCacheKey(meshPath);
		const auto meshAssetIterator = _meshAssets.find(cacheKey);
		if (meshAssetIterator != _meshAssets.end())
		{
			return meshAssetIterator->second.MeshData;
		}

		MeshAssetRecord meshAssetRecord;
		meshAssetRecord.SourcePath = meshPath.lexically_normal();
		meshAssetRecord.MeshData = std::move(meshData);

		const auto insertedIterator = _meshAssets.emplace(std::move(cacheKey), std::move(meshAssetRecord)).first;
		return insertedIterator->second.MeshData;
	}

	size_t AssetManager::GetLoadedMeshCount() const
	{
		return _meshAssets.size();
	}
#pragma endregion Mesh Cache
}