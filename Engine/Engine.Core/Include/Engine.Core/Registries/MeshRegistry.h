// MeshRegistry.h

#pragma once

#include <Engine.Core/Types/MeshTypes.h>
#include <Engine.Core/AssetHandles.h>
#include <Engine.Core/AssetLocators.h>
#include <Engine.Core/Registries/AssetRegistryBase.h>

namespace Engine::Core
{
	/// Concrete asset registry for mesh resources.
	///
	/// Extends AssetRegistryBase with storage for loaded mesh data.
	/// Each registered path is assigned a MeshHandle whose underlying value
	/// serves as a direct index into the internal record array, allowing O(1)
	/// lookup by handle.
	///
	/// Registration and loading are intentionally decoupled:
	///   - Register() - allocates a handle and a slot, but does not load any data.
	///   - Cache() - stores already-loaded mesh data into a registered slot.
	///
	/// This allows paths to be pre-registered at startup while actual mesh data
	/// is loaded lazily or asynchronously on demand.
	class MeshRegistry : public AssetRegistryBase
	{
	private:
#pragma region Internal Types
		/// Internal storage record for a single mesh asset.
		struct Record
		{
			/// The resolved locator identifying the source file and sub-asset index.
			/// Set during Register() and never modified afterwards.
			MeshAssetLocator Locator;

			/// The loaded mesh data, or nullptr if the mesh has not been loaded yet
			/// (registered but not cached) or has been unloaded.
			std::shared_ptr<Mesh> MeshData;
		};
#pragma endregion Internal Types

#pragma region Fields
		/// Contiguous array of mesh records. The index of each element corresponds
		/// to the handle value assigned during registration.
		std::vector<Record> _meshAssets;
#pragma endregion Fields

		/// Builds a normalized wide-string cache key from a mesh asset locator.
		/// Appends a "#mesh:<index>" suffix for locators with a sub-asset index,
		/// ensuring that different sub-assets from the same file get distinct keys.
		[[nodiscard]] std::wstring BuildMeshCacheKey(const MeshAssetLocator& locator) const;

	protected:
		/// Returns the display name of this registry, used in log messages.
		[[nodiscard]] const wchar_t* GetRegistryName() const override;

		/// Resolves a raw input path to a normalized source path suitable for loading.
		[[nodiscard]] std::filesystem::path ResolveSourcePath(const std::filesystem::path& path) const override;

	public:
		/// Registers a mesh path and returns a stable handle for it.
		/// If the path is already registered, returns the existing handle without
		/// allocating a new slot. Does not load any mesh data.
		[[nodiscard]] MeshHandle Register(const std::filesystem::path& path);

		/// Registers a mesh locator and returns a stable handle for it.
		/// If the locator is already registered, returns the existing handle without
		/// allocating a new slot. Does not load any mesh data.
		[[nodiscard]] MeshHandle Register(const MeshAssetLocator& locator);

		/// Returns the handle assigned to the given path,
		/// or an invalid handle if the path has not been registered.
		[[nodiscard]] MeshHandle FindHandle(const std::filesystem::path& path) const;

		/// Returns the handle assigned to the given locator,
		/// or an invalid handle if the locator has not been registered.
		[[nodiscard]] MeshHandle FindHandle(const MeshAssetLocator& locator) const;

		/// Returns the mesh data associated with the given handle,
		/// or nullptr if the handle is invalid or the mesh has not been cached yet.
		[[nodiscard]] std::shared_ptr<const Mesh> GetMesh(MeshHandle handle) const;

		/// Looks up a mesh by file path and returns its data,
		/// or nullptr if the path is not registered or the mesh has not been cached.
		[[nodiscard]] std::shared_ptr<const Mesh> FindMesh(const std::filesystem::path& path) const;

		/// Looks up a mesh by locator and returns its data,
		/// or nullptr if the locator is not registered or the mesh has not been cached.
		[[nodiscard]] std::shared_ptr<const Mesh> FindMesh(const MeshAssetLocator& locator) const;

		/// Stores loaded mesh data into the slot identified by the given handle.
		/// Returns the stored mesh pointer, allowing use in assignment expressions.
		/// The handle must have been obtained from a prior call to Register().
		[[nodiscard]] std::shared_ptr<const Mesh> Cache(const MeshHandle handle, std::shared_ptr<Mesh> data);

		/// Registers the given path (if not already registered) and stores
		/// the provided mesh data into the resulting slot in a single call.
		/// Equivalent to calling Register() followed by Cache().
		/// Returns the stored mesh pointer.
		[[nodiscard]] std::shared_ptr<const Mesh> Cache(const std::filesystem::path& path, std::shared_ptr<Mesh> data);

		/// Registers the given locator (if not already registered) and stores
		/// the provided mesh data into the resulting slot in a single call.
		/// Equivalent to calling Register() followed by Cache().
		/// Returns the stored mesh pointer, or nullptr if registration fails.
		[[nodiscard]] std::shared_ptr<const Mesh> Cache(const MeshAssetLocator& locator, std::shared_ptr<Mesh> data);

		/// Returns the source path associated with the given handle.
		/// The handle must have been obtained from a prior call to Register().
		/// Returns an empty path if the handle is invalid or out of range.
		[[nodiscard]] std::filesystem::path GetSourcePath(MeshHandle handle) const;

		/// Returns the resolved locator associated with the given handle.
		/// The handle must have been obtained from a prior call to Register().
		/// Returns a default-constructed locator if the handle is invalid or out of range.
		[[nodiscard]] MeshAssetLocator GetLocator(MeshHandle handle) const;

		/// Returns true if the mesh at the given path has been registered
		/// and its data is currently loaded in memory.
		[[nodiscard]] bool IsLoaded(const std::filesystem::path& path) const;

		/// Returns the number of mesh slots that currently have data loaded in memory.
		/// This may be less than GetRegisteredCount() if some meshes are registered
		/// but not yet cached, or have been unloaded.
		[[nodiscard]] size_t GetLoadedCount() const;

		/// Releases the mesh data associated with the given handle,
		/// freeing its memory while keeping the handle and path registration intact.
		/// The slot can be re-populated later via Cache().
		void Unload(MeshHandle handle);

		/// Releases all mesh data and clears all mesh registrations.
		void UnloadAll() override;
	};
}