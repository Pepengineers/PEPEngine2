// SceneRegistry.h

#pragma once

#include <Engine.Core/AssetHandles.h>
#include <Engine.Core/Types/SceneTypes.h>
#include <Engine.Core/Registries/AssetRegistryBase.h>

namespace Engine::Core
{
	class MeshRegistry;
	
	/// Concrete asset registry for scene resources.
	///
	/// Extends AssetRegistryBase with storage for loaded scene data.
	/// Each registered path is assigned a SceneHandle whose underlying value
	/// serves as a direct index into the internal record array, allowing O(1)
	/// lookup by handle.
	///
	/// Registration and loading are intentionally decoupled:
	///   - Register() - allocates a handle and a slot, but does not load any data.
	///   - Cache() - stores already-loaded scene data into a registered slot.
	///
	/// This allows paths to be pre-registered at startup while actual scene data
	/// is loaded lazily or asynchronously on demand.
	class SceneRegistry : public AssetRegistryBase
	{
	public:
		/// Registers a scene path and returns a stable handle for it.
		/// If the path is already registered, returns the existing handle without
		/// allocating a new slot. Does not load any scene data.
		[[nodiscard]] SceneHandle Register(const std::filesystem::path& path);

		/// Returns the handle assigned to the given path,
		/// or an invalid handle if the path has not been registered.
		[[nodiscard]] SceneHandle FindHandle(const std::filesystem::path& path) const;

		/// Returns the scene data associated with the given handle,
		/// or nullptr if the handle is invalid or the scene has not been cached yet.
		[[nodiscard]] std::shared_ptr<const SceneAsset> GetScene(SceneHandle handle) const;

		/// Looks up a scene by file path and returns its data,
		/// or nullptr if the path is not registered or the scene has not been cached.
		[[nodiscard]] std::shared_ptr<const SceneAsset> FindScene(const std::filesystem::path& path) const;

		/// Loads the scene from the given path, importing it from disk if needed.
		/// If the scene is already cached, returns the existing instance without re-importing.
		/// Registers the path automatically if it has not been registered yet.
		/// The provided MeshRegistry is forwarded to the scene importer, which registers
		/// and loads any meshes referenced by the scene during import.
		/// Returns nullptr if the path is empty, registration fails, or import fails.
		[[nodiscard]] std::shared_ptr<const SceneAsset> Load(const std::filesystem::path& path, MeshRegistry& meshRegistry);

		/// Stores loaded scene data into the slot identified by the given handle.
		/// Returns the stored scene pointer, allowing use in assignment expressions.
		/// The handle must have been obtained from a prior call to Register().
		[[nodiscard]] std::shared_ptr<const SceneAsset> Cache(SceneHandle handle, std::shared_ptr<SceneAsset> data);

		/// Registers the given path (if not already registered) and stores
		/// the provided scene data into the resulting slot in a single call.
		/// Equivalent to calling Register() followed by Cache().
		/// Returns the stored scene pointer, or nullptr if registration fails.
		[[nodiscard]] std::shared_ptr<const SceneAsset> Cache(const std::filesystem::path& path, std::shared_ptr<SceneAsset> data);

		/// Returns the source path associated with the given handle.
		/// The handle must have been obtained from a prior call to Register().
		/// Returns an empty path if the handle is invalid or out of range.
		[[nodiscard]] std::filesystem::path GetSourcePath(SceneHandle handle) const;

		/// Returns true if the scene at the given path has been registered
		/// and its data is currently loaded in memory.
		[[nodiscard]] bool IsLoaded(const std::filesystem::path& path) const;

		/// Returns the number of scene slots that currently have data loaded in memory.
		/// This may be less than GetRegisteredCount() if some scenes are registered
		/// but not yet cached, or have been unloaded.
		[[nodiscard]] size_t GetLoadedCount() const;

		/// Releases the scene data associated with the given handle,
		/// freeing its memory while keeping the handle and path registration intact.
		/// The slot can be re-populated later via Cache().
		void Unload(SceneHandle handle);

		/// Releases all scene data and clears all scene registrations.
		void UnloadAll() override;

	protected:
		/// Returns the display name of this registry, used in log messages.
		[[nodiscard]] const wchar_t* GetRegistryName() const override;

		/// Resolves a raw input path to a normalized source path suitable for loading.
		[[nodiscard]] std::filesystem::path ResolveSourcePath(const std::filesystem::path& path) const override;

	private:
#pragma region Internal Types
		/// Internal storage record for a single scene asset.
		struct Record
		{
			/// The resolved, normalized path to the scene source file.
			/// Set during Register() and never modified afterward.
			std::filesystem::path SourcePath;

			/// The loaded scene data, or nullptr if the scene has not been loaded yet
			/// (registered but not cached) or has been unloaded.
			std::shared_ptr<SceneAsset> SceneData;
		};
#pragma endregion Internal Types

#pragma region Fields
		/// Contiguous array of scene records. The index of each element corresponds
		/// to the handle value assigned during registration.
		std::vector<Record> _sceneAssets;
#pragma endregion Fields
	};
}