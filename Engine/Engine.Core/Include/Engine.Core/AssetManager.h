// AssetManager.h

#pragma once

#include <Engine.Core/Registries/MeshRegistry.h>
#include <Engine.Core/Registries/TextureRegistry.h>

#include <filesystem>
#include <string>
#include <unordered_map>

namespace Engine::Core
{
	/// Central cache for CPU-side asset data.
	class AssetManager
	{
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

		[[nodiscard]] std::shared_ptr<const Mesh> LoadMesh(const std::filesystem::path& path);

		/// Returns the mesh registry.
		[[nodiscard]] MeshRegistry& Meshes();
		/// Returns the mesh registry.
		[[nodiscard]] const MeshRegistry& Meshes() const;

		/// Returns the texture registry.
		[[nodiscard]] TextureRegistry& Textures();
		/// Returns the texture registry.
		[[nodiscard]] const TextureRegistry& Textures() const;

	private:
#pragma region Fields
		MeshRegistry _meshRegistry;
		TextureRegistry _textureRegistry;
#pragma endregion Fields

		AssetManager() = default;
		~AssetManager() = default;
	};
}