// TextureRegistry.h

#pragma once

#include <Engine.Core/Types/TextureTypes.h>
#include <Engine.Core/AssetHandles.h>
#include <Engine.Core/Registries/TypedAssetRegistry.h>

namespace Engine::Core
{
	/// Concrete asset registry for texture resources.
	///
	/// Builds on TypedAssetRegistry to reuse the common typed storage and lifecycle logic:
	///   - path registration;
	///   - handle-to-record lookup;
	///   - caching and unloading loaded texture data;
	///   - source-path queries and loaded-count tracking.
	///
	/// Unlike MeshRegistry, textures do not need custom locator metadata:
	/// the resolved source path itself is sufficient as the per-record metadata.
	class TextureRegistry : public TypedAssetRegistry<Texture, TextureHandle, std::filesystem::path>
	{
	public:
		/// Returns the texture data associated with the given handle,
		/// or nullptr if the handle is invalid or the texture has not been cached yet.
		[[nodiscard]] const Texture* GetTexture(TextureHandle handle) const;

		/// Looks up a texture by file path and returns its data,
		/// or nullptr if the path is not registered or the texture has not been cached.
		[[nodiscard]] const Texture* FindTexture(const std::filesystem::path& path) const;

		/// Loads a texture from cache or from disk and caches the result.
		[[nodiscard]] const Texture* Load(const std::filesystem::path& path);

		/// Loads a texture from cache or from disk and returns the default texture on failure.
		[[nodiscard]] const Texture* LoadOrDefault(const std::filesystem::path& path);

		/// Returns the lazily-created default error texture.
		[[nodiscard]] const Texture* GetDefaultTexture();
		
	protected:
		/// Returns the display name of this registry, used in log messages.
		[[nodiscard]] const wchar_t* GetRegistryName() const override;

		/// Resolves a raw texture path to a normalized source path suitable for loading.
		[[nodiscard]] std::filesystem::path ResolveSourcePath(const std::filesystem::path& path) const override;

		/// Creates texture metadata for a newly registered path.
		/// For textures the metadata is just the resolved source path itself.
		[[nodiscard]] std::filesystem::path MakeMetadataForRegisteredPath(const std::filesystem::path& resolvedSourcePath) const override;

		/// Extracts the canonical source path from stored texture metadata.
		[[nodiscard]] std::filesystem::path GetSourcePathFromMetadata(const std::filesystem::path& metadata) const override;

		/// Returns the singular asset type name used in log messages.
		[[nodiscard]] const wchar_t* GetAssetTypeName() const override;

	private:
		/// Handle of the file-backed default texture if it was loaded through the registry.
		TextureHandle _defaultTextureHandle;

		/// Built-in fallback texture returned when even the file-backed default texture
		/// is missing or failed to load. Owned locally because it is not registered.
		std::unique_ptr<Texture> _fallbackDefaultTexture;
	};
}