// MeshRegistry.h

#pragma once

#include <Engine.Core/Types/MeshTypes.h>
#include <Engine.Core/AssetHandles.h>
#include <Engine.Core/AssetLocators.h>
#include <Engine.Core/Registries/AssetRegistryBase.h>
#include <Engine.Core/Registries/TypedAssetRegistry.h>

namespace Engine::Core
{
	/// Concrete asset registry for mesh resources.
	///
	/// Builds on TypedAssetRegistry to reuse the common typed storage and lifecycle logic:
	///   - path registration;
	///   - handle-to-record lookup;
	///   - caching and unloading loaded mesh data;
	///   - source-path queries and loaded-count tracking.
	///
	/// MeshRegistry extends that common behavior with mesh-specific functionality:
	///   - MeshAssetLocator-based registration and lookup;
	///   - support for addressing sub-assets inside multi-mesh files;
	///   - mesh-specific cache key construction.
	class MeshRegistry : public TypedAssetRegistry<Mesh, MeshHandle, MeshAssetLocator>
	{
	public:
		/// Registers a mesh locator and returns a stable handle for it.
		/// If the locator is already registered, returns the existing handle
		/// without allocating a new slot. Does not load any mesh data.
		[[nodiscard]] MeshHandle Register(const MeshAssetLocator& locator);

		/// Returns the handle assigned to the given mesh locator,
		/// or an invalid handle if the locator has not been registered.
		[[nodiscard]] MeshHandle FindHandle(const MeshAssetLocator& locator) const;

		/// Returns the mesh data associated with the given handle,
		/// or nullptr if the handle is invalid or the mesh has not been cached yet.
		[[nodiscard]] const Mesh* GetMesh(MeshHandle handle) const;

		/// Looks up a mesh by file path and returns its data,
		/// or nullptr if the path is not registered or the mesh has not been cached.
		[[nodiscard]] const Mesh* FindMesh(const std::filesystem::path& path) const;

		/// Looks up a mesh by locator and returns its data,
		/// or nullptr if the locator is not registered or the mesh has not been cached.
		[[nodiscard]] const Mesh* FindMesh(const MeshAssetLocator& locator) const;
		
		/// Loads one standalone mesh asset from path.
		/// Equivalent to Load() with a locator that does not specify SubAssetIndex.
        [[nodiscard]] const Mesh* Load(const std::filesystem::path& path);

        /// Loads one logical mesh asset addressed by locator.
        /// If locator contains SubAssetIndex, loads only that mesh sub-asset from the source file.
        [[nodiscard]] const Mesh* Load(const MeshAssetLocator& locator);

		/// Loads one logical mesh asset addressed by locator and returns its handle via outHandle.
		/// If the asset is already registered, outHandle receives the existing handle.
		/// On failure returns nullptr and resets outHandle to an invalid handle.
		[[nodiscard]] const Mesh* Load(const MeshAssetLocator& locator, MeshHandle& outHandle);

		/// Returns the resolved mesh locator associated with the given handle.
		/// The handle must have been obtained from a prior call to Register().
		/// Returns a default-constructed locator if the handle is invalid or out of range.
		[[nodiscard]] MeshAssetLocator GetLocator(MeshHandle handle) const;

	protected:
		/// Returns the display name of this registry, used in log messages.
		[[nodiscard]] const wchar_t* GetRegistryName() const override;

		/// Resolves a raw mesh path to a normalized source path suitable for loading.
		[[nodiscard]] std::filesystem::path ResolveSourcePath(const std::filesystem::path& path) const override;

		/// Creates mesh metadata for a newly registered path.
		/// For plain path-based registration this produces a locator without a sub-asset index.
		[[nodiscard]] MeshAssetLocator MakeMetadataForRegisteredPath(const std::filesystem::path& resolvedSourcePath) const override;

		/// Extracts the canonical source path from stored mesh metadata.
		[[nodiscard]] std::filesystem::path GetSourcePathFromMetadata(const MeshAssetLocator& metadata) const override;

		/// Returns the singular asset type name used in log messages.
		[[nodiscard]] const wchar_t* GetAssetTypeName() const override;

	private:
		/// Builds a normalized wide-string cache key from a mesh asset locator.
		/// Appends a "#mesh:<index>" suffix for locators with a sub-asset index,
		/// ensuring that different sub-assets from the same file get distinct keys.
		[[nodiscard]] static std::wstring BuildMeshCacheKey(const MeshAssetLocator& locator);
	};
}