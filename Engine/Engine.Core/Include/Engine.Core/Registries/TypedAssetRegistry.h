#pragma once

#include <Engine.Core/Registries/AssetRegistryBase.h>

#include <cassert>
#include <string>
#include <vector>

namespace Engine::Core
{
	/// Generic typed asset registry built on top of AssetRegistryBase.
	///
	/// AssetRegistryBase manages the common path/key -> uint32 handle mapping.
	/// TypedAssetRegistry adds the typed layer on top of it:
	///   - per-handle storage for registry-specific metadata;
	///   - per-handle storage for loaded asset data;
	///   - common implementations of Cache(), GetSourcePath(), IsLoaded(),
	///     GetLoadedCount(), Unload(), and UnloadAll().
	///
	/// Registration and loading are intentionally decoupled:
	///   - Register(path) allocates a handle and creates a typed record;
	///   - Cache(handle, data) stores already-loaded asset data into that record.
	///
	/// Derived registries are responsible only for the asset-specific parts:
	///   - path resolution;
	///   - metadata construction from a resolved source path;
	///   - extracting the source path from stored metadata;
	///   - providing the singular asset type name for log messages.
	template<typename TAsset, typename THandle, typename TMetadata>
	class TypedAssetRegistry : public AssetRegistryBase
	{
	public:
		/// Registers the given asset path and returns a typed handle for it.
		/// If the path is already registered, returns the existing handle
		/// without allocating a new slot or duplicating metadata.
		[[nodiscard]] THandle Register(const std::filesystem::path& path)
		{
			const RegistrationResult registrationResult = RegisterPath(path);
			if (registrationResult.HandleValue == InvalidHandleValue)
			{
				return {};
			}

			return FinalizeRegistration(registrationResult);
		}

		/// Returns the typed handle assigned to the given path,
		/// or an invalid handle if the path has not been registered.
		[[nodiscard]] THandle FindHandle(const std::filesystem::path& path) const
		{
			const std::uint32_t handleValue = FindHandleValue(path);
			if (handleValue == InvalidHandleValue)
			{
				return {};
			}

			return THandle(handleValue);
		}

		/// Stores loaded asset data into the slot identified by the given handle.
		/// Returns the stored pointer, allowing use in assignment expressions.
		/// The handle must have been obtained from a prior call to Register().
		[[nodiscard]] std::shared_ptr<const TAsset> Cache(const THandle handle, std::shared_ptr<TAsset> data)
		{
			const std::wstring assetName = GetAssetName();

			if (data == nullptr)
			{
				WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Failed to cache " + assetName + L": null " + assetName + L" data.\n");
				assert(false);
				return nullptr;
			}

			Record* record = TryGetRecord(handle, L"cache " + assetName);
			if (record == nullptr)
			{
				assert(false);
				return nullptr;
			}

			if (record->AssetData != nullptr)
			{
				WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] " + assetName + L" data already cached for handle " +
					std::to_wstring(handle.GetValue()) + L".\n");
				return record->AssetData;
			}

			record->AssetData = std::move(data);
			++_loadedCount;

			WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Cached " + assetName + L" data for handle " +
				std::to_wstring(handle.GetValue()) + L". Path: " + GetSourcePathFromMetadata(record->Metadata).generic_wstring() + L"\n");

			return record->AssetData;
		}

		/// Registers the given path (if not already registered) and stores
		/// the provided asset data into the resulting slot in a single call.
		/// Equivalent to calling Register() followed by Cache().
		/// Returns the stored pointer, or nullptr if registration fails.
		[[nodiscard]] std::shared_ptr<const TAsset> Cache(const std::filesystem::path& path, std::shared_ptr<TAsset> data)
		{
			const THandle handle = Register(path);
			if (!handle.IsValid())
			{
				return nullptr;
			}

			return Cache(handle, std::move(data));
		}

		/// Returns the source path associated with the given handle.
		/// The handle must have been obtained from a prior call to Register().
		/// Returns an empty path if the handle is invalid or out of range.
		[[nodiscard]] std::filesystem::path GetSourcePath(const THandle handle) const
		{
			const Record* record = TryGetRecord(handle, L"get source path to the " + GetAssetName() + L" handle");
			if (record == nullptr)
			{
				return {};
			}

			return GetSourcePathFromMetadata(record->Metadata);
		}

		/// Returns true if the asset at the given path has been registered
		/// and its data is currently loaded in memory.
		[[nodiscard]] bool IsLoaded(const std::filesystem::path& path) const
		{
			const THandle handle = FindHandle(path);
			if (!handle.IsValid())
			{
				return false;
			}

			return GetAsset(handle) != nullptr;
		}

		/// Returns the number of slots that currently have asset data loaded in memory.
		/// This may be less than GetRegisteredCount() if some assets are registered
		/// but not yet cached, or have been unloaded.
		[[nodiscard]] size_t GetLoadedCount() const
		{
			return _loadedCount;
		}

		/// Releases the asset data associated with the given handle,
		/// freeing its memory while keeping the handle and path registration intact.
		/// The slot can be re-populated later via Cache().
		void Unload(const THandle handle)
		{
			const std::wstring assetName = GetAssetName();

			Record* record = TryGetRecord(handle, L"unload " + assetName);
			if (record == nullptr)
			{
				return;
			}

			if (record->AssetData == nullptr)
			{
				WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] " + assetName + L" handle " +
					std::to_wstring(handle.GetValue()) + L" has no cached data to unload.\n");
				return;
			}

			--_loadedCount;
			record->AssetData.reset();

			WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Unloaded " + assetName + L" data for handle " +
				std::to_wstring(handle.GetValue()) + L".\n");
		}

		/// Releases all loaded asset data and clears all path registrations
		/// managed by this typed registry.
		void UnloadAll() override
		{
			_records.clear();
			_loadedCount = 0;
			ClearRegistry();

			WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Cleared all " + GetAssetName() + L" registry entries.\n");
		}

	protected:
		/// Internal storage record for a single registered asset.
		struct Record
		{
			/// Registry-specific metadata associated with the registered slot.
			/// Examples:
			///   - std::filesystem::path for textures;
			///   - MeshAssetLocator for meshes.
			TMetadata Metadata{};

			/// The loaded asset data, or nullptr if the asset has not been loaded yet
			/// (registered but not cached) or has been unloaded.
			std::shared_ptr<TAsset> AssetData;
		};

		TypedAssetRegistry() = default;

		/// Returns the loaded asset data associated with the given handle,
		/// or nullptr if the handle is invalid or the asset has not been cached yet.
		[[nodiscard]] std::shared_ptr<const TAsset> GetAsset(const THandle handle) const
		{
			const Record* record = TryGetRecord(handle, L"get " + GetAssetName());
			if (record == nullptr)
			{
				return nullptr;
			}

			return record->AssetData;
		}

		/// Completes registration by creating a typed record for a newly assigned handle.
		/// If the path was already registered, simply returns the existing typed handle.
		[[nodiscard]] THandle FinalizeRegistration(const RegistrationResult& registrationResult, TMetadata metadata)
		{
			if (registrationResult.HandleValue == InvalidHandleValue)
			{
				return {};
			}

			if (!registrationResult.bAlreadyRegistered)
			{
				assert(_records.size() == static_cast<size_t>(registrationResult.HandleValue));

				Record record;
				record.Metadata = std::move(metadata);

				_records.push_back(std::move(record));
			}

			return THandle(registrationResult.HandleValue);
		}

		/// Completes registration using metadata produced from the resolved source path
		/// stored in the given RegistrationResult.
		/// This overload is used by plain path-based registration flows.
		[[nodiscard]] THandle FinalizeRegistration(const RegistrationResult& registrationResult)
		{
			if (registrationResult.HandleValue == InvalidHandleValue)
			{
				return {};
			}

			return FinalizeRegistration(registrationResult, MakeMetadataForRegisteredPath(registrationResult.SourcePath));
		}

		/// Validates the given handle and returns the corresponding record.
		/// Logs a descriptive failure message and returns nullptr on invalid input.
		[[nodiscard]] const Record* TryGetRecord(const THandle handle, const std::wstring& operation) const
		{
			if (!handle.IsValid())
			{
				WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Failed to " + operation + L": invalid handle.\n");
				return nullptr;
			}

			const size_t recordIndex = static_cast<size_t>(handle.GetValue());
			if (recordIndex >= _records.size())
			{
				WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Failed to " + operation + L": handle index out of range: " +
					std::to_wstring(handle.GetValue()) + L"\n");
				return nullptr;
			}

			return &_records[recordIndex];
		}

		/// Mutable overload of TryGetRecord().
		/// Reuses the const implementation to keep validation logic in one place.
		[[nodiscard]] Record* TryGetRecord(const THandle handle, const std::wstring& operation)
		{
			return const_cast<Record*>(static_cast<const TypedAssetRegistry&>(*this).TryGetRecord(handle, operation));
		}

		/// Creates registry-specific metadata for a newly registered path.
		/// The input path is already resolved and normalized.
		[[nodiscard]] virtual TMetadata MakeMetadataForRegisteredPath(const std::filesystem::path& resolvedSourcePath) const = 0;

		/// Extracts the canonical source path from stored metadata.
		/// Used by the common typed layer for logging and GetSourcePath().
		[[nodiscard]] virtual std::filesystem::path GetSourcePathFromMetadata(const TMetadata& metadata) const = 0;

		/// Returns the singular asset type name used in log messages,
		/// for example L"mesh" or L"texture".
		[[nodiscard]] virtual const wchar_t* GetAssetTypeName() const = 0;

	private:
		/// Returns the singular asset type name as a std::wstring.
		[[nodiscard]] std::wstring GetAssetName() const
		{
			return std::wstring(GetAssetTypeName());
		}

		/// Contiguous array of typed asset records.
		/// The index of each element corresponds to the handle value assigned during registration.
		std::vector<Record> _records;

		/// Number of records that currently have loaded asset data.
		size_t _loadedCount = 0;
	};
}