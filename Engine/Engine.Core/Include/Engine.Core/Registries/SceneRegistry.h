// SceneRegistry.h

#pragma once

#include <Engine.Core/AssetHandles.h>
#include <Engine.Core/Types/SceneTypes.h>
#include <Engine.Core/Registries/TypedAssetRegistry.h>

namespace Engine::Core
{
	class MeshRegistry;

	/// Concrete asset registry for scene resources.
	///
	/// Builds on TypedAssetRegistry to reuse the common typed storage and lifecycle logic:
	///   - path registration;
	///   - handle-to-record lookup;
	///   - caching and unloading loaded scene data;
	///   - source-path queries and loaded-count tracking.
	///
	/// Unlike MeshRegistry, scenes do not need custom locator metadata:
	/// the resolved source path itself is sufficient as the per-record metadata.
	class SceneRegistry : public TypedAssetRegistry<SceneAsset, SceneHandle, std::filesystem::path>
	{
	public:
		/// Returns the scene data associated with the given handle,
		/// or nullptr if the handle is invalid or the scene has not been cached yet.
		[[nodiscard]] const SceneAsset* GetScene(SceneHandle handle) const;

		/// Looks up a scene by file path and returns its data,
		/// or nullptr if the path is not registered or the scene has not been cached.
		[[nodiscard]] const SceneAsset* FindScene(const std::filesystem::path& path) const;

		/// Loads the scene from the given path, importing it from disk if needed.
		/// If the scene is already cached, returns the existing instance without re-importing.
		/// Registers the path automatically if it has not been registered yet.
		/// The provided MeshRegistry is forwarded to the scene importer, which registers
		/// and loads any meshes referenced by the scene during import.
		/// Returns nullptr if the path is empty, registration fails, or import fails.
		[[nodiscard]] const SceneAsset* Load(const std::filesystem::path& path, MeshRegistry& meshRegistry);

	protected:
		/// Returns the display name of this registry, used in log messages.
		[[nodiscard]] const wchar_t* GetRegistryName() const override;

		/// Resolves a raw input path to a normalized source path suitable for loading.
		[[nodiscard]] std::filesystem::path ResolveSourcePath(const std::filesystem::path& path) const override;

		/// Creates scene metadata for a newly registered path.
		/// For scenes the metadata is just the resolved source path itself.
		[[nodiscard]] std::filesystem::path MakeMetadataForRegisteredPath(const std::filesystem::path& resolvedSourcePath) const override;

		/// Extracts the canonical source path from stored scene metadata.
		[[nodiscard]] std::filesystem::path GetSourcePathFromMetadata(const std::filesystem::path& metadata) const override;

		/// Rebuilds the registry cache key from stored scene metadata.
		/// For scenes this is just the normalized source path key.
		[[nodiscard]] std::wstring GetCacheKeyFromMetadata(const std::filesystem::path& metadata) const override;

		/// Returns the singular asset type name used in log messages.
		[[nodiscard]] const wchar_t* GetAssetTypeName() const override;
	};
}