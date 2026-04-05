// TextureRegistry.h

#pragma once

#include <Engine.Core/Types/TextureTypes.h>
#include <Engine.Core/AssetHandles.h>
#include <Engine.Core/Registries/AssetRegistryBase.h>

namespace Engine::Core
{
	/// Concrete asset registry for texture resources.
	///
	/// Extends AssetRegistryBase with storage for loaded texture data.
	/// Each registered path is assigned a TextureHandle whose underlying value
	/// serves as a direct index into the internal record array, allowing O(1)
	/// lookup by handle.
	///
	/// Registration and loading are intentionally decoupled:
	///   - Register() - allocates a handle and a slot, but does not load any data.
	///   - Cache() - stores already-loaded texture data into a registered slot.
	///
	/// This allows paths to be pre-registered at startup while actual texture data
	/// is loaded lazily or asynchronously on demand.
	class TextureRegistry : public AssetRegistryBase
	{
	private:
#pragma region Internal Types
		/// Internal storage record for a single texture asset.
		struct Record
		{
			/// The resolved, normalized path to the texture source file.
			std::filesystem::path SourcePath;

			/// The loaded texture data, or nullptr if the texture has not been loaded yet
			/// (registered but not cached) or has been unloaded.
			std::shared_ptr<Texture> TextureData;
		};
#pragma endregion Internal Types

#pragma region Fields
		/// Contiguous array of texture records. The index of each element corresponds
		/// to the handle value assigned during registration.
		std::vector<Record> _textureAssets;
#pragma endregion Fields

	protected:
		/// Returns the display name of this registry, used in log messages.
		[[nodiscard]] const wchar_t* GetRegistryName() const override;

		/// Resolves a raw input path to a normalized source path suitable for loading.
		[[nodiscard]] std::filesystem::path ResolveSourcePath(const std::filesystem::path& path) const override;

	public:
		/// Registers a texture path and returns a stable handle for it.
		/// If the path is already registered, returns the existing handle without
		/// allocating a new slot. Does not load any texture data.
		[[nodiscard]] TextureHandle Register(const std::filesystem::path& path);

		/// Returns the handle assigned to the given path,
		/// or an invalid handle if the path has not been registered.
		[[nodiscard]] TextureHandle FindHandle(const std::filesystem::path& path) const;

		/// Returns the texture data associated with the given handle,
		/// or nullptr if the handle is invalid or the texture has not been cached yet.
		[[nodiscard]] std::shared_ptr<const Texture> GetTexture(TextureHandle handle) const;

		/// Looks up a texture by file path and returns its data,
		/// or nullptr if the path is not registered or the texture has not been cached.
		[[nodiscard]] std::shared_ptr<const Texture> FindTexture(const std::filesystem::path& path) const;

		/// Stores loaded texture data into the slot identified by the given handle.
		/// Returns the stored texture pointer, allowing use in assignment expressions.
		/// The handle must have been obtained from a prior call to Register().
		[[nodiscard]] std::shared_ptr<const Texture> Cache(const TextureHandle handle, std::shared_ptr<Texture> data);

		/// Registers the given path (if not already registered) and stores
		/// the provided texture data into the resulting slot in a single call.
		/// Equivalent to calling Register() followed by Cache().
		/// Returns the stored texture pointer.
		[[nodiscard]] std::shared_ptr<const Texture> Cache(const std::filesystem::path& path, std::shared_ptr<Texture> data);

		/// Returns the source path associated with the given handle.
		/// The handle must have been obtained from a prior call to Register().
		/// Returns an empty path if the handle is invalid or out of range.
		[[nodiscard]] std::filesystem::path GetSourcePath(TextureHandle handle) const;

		/// Returns true if the texture at the given path has been registered
		/// and its data is currently loaded in memory.
		[[nodiscard]] bool IsLoaded(const std::filesystem::path& path) const;

		/// Returns the number of texture slots that currently have data loaded in memory.
		/// This may be less than GetRegisteredCount() if some textures are registered
		/// but not yet cached, or have been unloaded.
		[[nodiscard]] size_t GetLoadedCount() const;

		/// Releases the texture data associated with the given handle,
		/// freeing its memory while keeping the handle and path registration intact.
		/// The slot can be re-populated later via Cache().
		void Unload(TextureHandle handle);
		
		/// Releases all texture data and clears all texture registrations.
		void UnloadAll() override;
	};
}