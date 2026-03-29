// AssetManager.h

#pragma once

#include <Engine.Core/MeshTypes.h>
#include <Engine.Core/AssetHandles.h>

#include <filesystem>
#include <string>
#include <unordered_map>

namespace Engine::Core
{
	/// Central cache for CPU-side asset data.
	class AssetManager
	{
	private:
#pragma region Internal Types
		/// A single mesh entry stored by AssetManager.
		struct MeshAssetRecord
		{
			// Source path used to create this cache entry.
			std::filesystem::path SourcePath;

			// Shared CPU-side mesh data.
			std::shared_ptr<Mesh> MeshData;
		};

		/// A single texture entry stored by AssetManager.
		struct TextureAssetRecord
		{
			// Source path used to create this cache entry.
			std::filesystem::path SourcePath;
		};
#pragma endregion Internal Types

#pragma region Fields
		/// Maps normalized mesh paths to mesh handles.
		std::unordered_map<std::wstring, MeshHandle> _meshHandlesByPath;

		/// Stores mesh asset records by handle index.
		std::vector<MeshAssetRecord> _meshAssets;

		/// Maps normalized texture paths to texture handles.
		std::unordered_map<std::wstring, TextureHandle> _textureHandlesByPath;

		/// Stores texture asset records by handle index.
		std::vector<TextureAssetRecord> _textureAssets;
#pragma endregion Fields

#pragma region Private Methods
		AssetManager() = default;
		~AssetManager() = default;

		/// Resolves a relative or absolute mesh path to a normalized source path.
		static std::filesystem::path ResolveMeshSourcePath(const std::filesystem::path& meshPath);

		/// Resolves a relative or absolute texture path to a normalized source path.
		static std::filesystem::path ResolveTextureSourcePath(const std::filesystem::path& texturePath);
		
		/// Builds a normalized cache key for mesh lookup.
		static std::wstring BuildMeshCacheKey(const std::filesystem::path& meshPath);

		/// Builds a normalized cache key for texture lookup.
		static std::wstring BuildTextureCacheKey(const std::filesystem::path& texturePath);
#pragma endregion Private Methods

	public:
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

#pragma region Meshes
		/// Registers a mesh path and returns a stable handle for it.
		[[nodiscard]] MeshHandle RegisterMesh(const std::filesystem::path& meshPath);

		/// Returns true if a mesh path is already registered.
		[[nodiscard]] bool IsMeshRegistered(const std::filesystem::path& meshPath) const;

		/// Returns a registered mesh handle or an invalid handle if not found.
		[[nodiscard]] MeshHandle FindMeshHandle(const std::filesystem::path& meshPath) const;

		/// Returns mesh data for a valid handle if it has already been cached.
		[[nodiscard]] std::shared_ptr<const Mesh> GetMesh(MeshHandle meshHandle) const;

		/// Stores mesh data in the slot referenced by meshHandle.
		[[nodiscard]] std::shared_ptr<const Mesh> CacheMesh(MeshHandle meshHandle, std::shared_ptr<Mesh> meshData);
		
		/// Returns true if a mesh path is registered and already has cached data.
		[[nodiscard]] bool IsMeshLoaded(const std::filesystem::path& meshPath) const;

		/// Returns cached mesh data if present; otherwise returns nullptr.
		[[nodiscard]] std::shared_ptr<const Mesh> FindMesh(const std::filesystem::path& meshPath) const;

		/// Registers a mesh path if needed and stores mesh data in its slot.
		[[nodiscard]] std::shared_ptr<const Mesh> CacheMesh(const std::filesystem::path& meshPath, std::shared_ptr<Mesh> meshData);

		/// Returns the number of registered mesh entries.
		[[nodiscard]] size_t GetRegisteredMeshCount() const;

		/// Returns the number of mesh entries that already contain cached data.
		[[nodiscard]] size_t GetLoadedMeshCount() const;
#pragma endregion Meshes

#pragma region Textures
		/// Registers a texture path and returns a stable handle for it.
		[[nodiscard]] TextureHandle RegisterTexture(const std::filesystem::path& texturePath);

		/// Returns true if a texture path is already registered.
		[[nodiscard]] bool IsTextureRegistered(const std::filesystem::path& texturePath) const;

		/// Returns a registered texture handle or an invalid handle if not found.
		[[nodiscard]] TextureHandle FindTextureHandle(const std::filesystem::path& texturePath) const;

		/// Returns the number of registered texture entries.
		[[nodiscard]] size_t GetRegisteredTextureCount() const;
#pragma endregion Textures
	};
}