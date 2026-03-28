// AssetManager.h

#pragma once

#include <Engine.Core/MeshTypes.h>

#include <filesystem>
#include <string>
#include <unordered_map>

namespace Engine::Core
{
	/// Central cache for CPU-side asset data.
	/// At this step it manages mesh assets only.
	class AssetManager
	{
	private:
#pragma region Internal Types
		/// A single entry in the AssetManager describing one loaded mesh.
		struct MeshAssetRecord
		{
			// Source path used to create this cache entry.
			std::filesystem::path SourcePath;

			// Shared CPU-side mesh data.
			std::shared_ptr<Mesh> MeshData;
		};
#pragma endregion Internal Types

#pragma region Fields
		/// Mesh cache.
		std::unordered_map<std::wstring, MeshAssetRecord> _meshAssets;
#pragma endregion Fields

#pragma region Private Methods
		AssetManager() = default;
		~AssetManager() = default;

		/// Builds a normalized cache key for mesh lookup.
		static std::wstring BuildMeshCacheKey(const std::filesystem::path& meshPath);
#pragma endregion Private Methods

	public:
#pragma region Public Methods
		// Prevents copying (AssetManager manager = AssetManager::GetInstance(); -> error)
		AssetManager(const AssetManager& rhs) = delete;
		// Prevents copy assigment (AssetManager manager; manager = AssetManager::GetInstance(); -> error)
		AssetManager& operator=(const AssetManager& rhs) = delete;
		// Prevents move constructor (AssetManager& manager = AssetManager::GetInstance(); AssetManager movedManager = std::move(manager); -> error)
		AssetManager(AssetManager&& rhs) = delete;
		// Prevents move assigment (AssetManager manager1; AssetManager manager2; manager1 = std::move(manager2); -> error)
		AssetManager& operator=(AssetManager&& rhs) = delete;

		/// Returns the global asset manager instance.
		static AssetManager& GetInstance();

		/// Returns true if a mesh with the same normalized path is already cached.
		[[nodiscard]] bool IsMeshLoaded(const std::filesystem::path& meshPath) const;

		/// Returns cached mesh data if present; otherwise returns nullptr.
		[[nodiscard]] std::shared_ptr<const Mesh> FindMesh(const std::filesystem::path& meshPath) const;

		/// Stores mesh data in cache or returns the existing cached entry.
		std::shared_ptr<const Mesh> CacheMesh(const std::filesystem::path& meshPath, std::shared_ptr<Mesh> meshData);

		/// Returns the number of currently cached meshes.
		[[nodiscard]] size_t GetLoadedMeshCount() const;
#pragma endregion Public Methods
	};
}