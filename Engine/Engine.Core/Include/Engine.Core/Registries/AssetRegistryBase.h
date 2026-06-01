// AssetRegistryBase.h

#pragma once

#include <string>
#include <unordered_map>
#include <filesystem>


namespace Engine::Core
{
	/// Abstract base class for asset registries.
	///
	/// Maintains a mapping from asset file paths to lightweight uint32_t handles,
	/// allowing the engine to reference assets by stable numeric IDs rather than
	/// raw path strings. Derived classes specialize behavior for a particular asset
	/// type (e.g. meshes, textures, audio) by implementing the pure virtual methods.
	///
	/// Each registry is a unique owner of its handle table - copying is disabled.
	/// Moving is allowed to support registry relocation (e.g. storing in containers).
	class AssetRegistryBase
	{
	public:
		/// Virtual destructor. Ensures correct cleanup when deleting through a base pointer.
		virtual ~AssetRegistryBase() = default;
	
		/// Copying a registry is disabled - each instance is the unique owner of its handle table.
		AssetRegistryBase(const AssetRegistryBase&) = delete;
		/// Copy assignment is disabled for the same reason as copy construction.
		AssetRegistryBase& operator=(const AssetRegistryBase&) = delete;

		/// Move constructor. Transfers ownership of the handle table to a new instance.
		/// Marked noexcept to enable efficient use in standard containers (e.g. std::vector).
		/// Example: DerivedRegistry r = std::move(otherRegistry);
		AssetRegistryBase(AssetRegistryBase&&) noexcept = default;
		/// Move assignment operator. Transfers ownership of the handle table.
		/// Example: registryA = std::move(registryB);
		AssetRegistryBase& operator=(AssetRegistryBase&&) noexcept = default;

		/// Returns true if the given path has already been registered in this registry.
		[[nodiscard]] bool IsRegistered(const std::filesystem::path& path) const;

		/// Returns the handle value assigned to the given path,
		/// or InvalidHandleValue if the path has not been registered.
		[[nodiscard]] std::uint32_t FindHandleValue(const std::filesystem::path& path) const;

		/// Returns the total number of paths currently registered in this registry.
		[[nodiscard]] size_t GetRegisteredCount() const;

		/// Unloads all assets tracked by this registry and clears its internal state.
		/// Must be implemented by derived classes to perform type-specific cleanup.
		virtual void UnloadAll() = 0;

		protected:
#pragma region Internal Types
		/// Sentinel value indicating an uninitialized or invalid handle.
		static constexpr std::uint32_t InvalidHandleValue = (std::numeric_limits<std::uint32_t>::max)();

		/// Result returned by RegisterPath(), describing the outcome of a registration attempt.
		struct RegistrationResult
		{
			/// The handle value assigned to the registered path,
			/// or InvalidHandleValue if registration failed.
			std::uint32_t HandleValue = InvalidHandleValue;

			/// The resolved, normalized source path used for the actual asset lookup.
			std::filesystem::path SourcePath;

			/// The normalized wide-string key used to look up this entry in the internal handle map.
			/// Derived from the resolved source path via BuildCacheKey().
			std::wstring CacheKey;

			/// True if the path was already registered before this call.
			/// When true, the existing handle is returned rather than a new one being allocated.
			bool bAlreadyRegistered = false;
		};
#pragma endregion Internal Types

#pragma region Fields
		/// Maps normalized path strings to their assigned handle values.
		std::unordered_map<std::wstring, std::uint32_t> _handlesByPath;

		/// Monotonic counter used to issue new handle values.
        std::uint32_t _nextHandleValue = 0;

		/// Stack of previously freed handle values that can be reused by future registrations.
		std::vector<std::uint32_t> _freeHandleValues;
#pragma endregion Fields

		/// Default constructor. Initializes an empty registry.
		/// Only callable from derived classes.
		AssetRegistryBase() = default;

		/// Returns a human-readable name for this registry, used in log messages.
		/// Must be implemented by each derived registry (e.g. L"MeshRegistry").
		[[nodiscard]] virtual const wchar_t* GetRegistryName() const = 0;

		/// Resolves a raw input path to a normalized source path suitable for loading.
		[[nodiscard]] virtual std::filesystem::path ResolveSourcePath(const std::filesystem::path& path) const = 0;

		/// Writes a formatted message to the registry-specific log output.
		static void WriteRegistryLog(const std::wstring& message);

		/// Builds a normalized wide-string cache key from a given path.
		/// The key is used as the lookup key in the internal handle map.
		[[nodiscard]] std::wstring BuildCacheKey(const std::filesystem::path& path) const;

		/// Builds a normalized wide-string cache key from an already resolved source path.
        [[nodiscard]] static std::wstring BuildCacheKeyFromResolvedPath(const std::filesystem::path& resolvedSourcePath);

		/// Registers a resolved, already-normalized path directly, bypassing the resolution step.
		/// Use this when the source path and cache key have already been computed.
		[[nodiscard]] RegistrationResult RegisterResolvedPath(const std::filesystem::path& sourcePath, const std::wstring& cacheKey);

		/// Registers a path and assigns it a new handle if not already registered.
		/// Returns a RegistrationResult describing the assigned handle, resolved path,
		/// and whether the path was already present in the registry.
		[[nodiscard]] RegistrationResult RegisterPath(const std::filesystem::path& path);

		/// Removes the given cache key from the registry and marks its handle value as reusable.
		/// Returns false if the key is empty or was not registered.
		[[nodiscard]] bool UnregisterKey(const std::wstring& cacheKey);

		/// Returns true if the given cache key is already present in the internal handle map.
		[[nodiscard]] bool IsRegisteredKey(const std::wstring& cacheKey) const;

		/// Returns the handle value assigned to the given cache key,
		/// or InvalidHandleValue if the key has not been registered.
		[[nodiscard]] std::uint32_t FindHandleValueByKey(const std::wstring& cacheKey) const;

		/// Removes all registered path-to-handle mappings from the registry.
		/// Does not unload any underlying assets - use UnloadAll() for that.
		void ClearRegistry();
	};
}