// MeshRegistry.cpp

#include <Engine.Core/Registries/MeshRegistry.h>
#include <Engine.Core/IO/MeshImporter.h>

namespace Engine::Core
{
	namespace
	{
		bool IsAssetsRootRelative(const std::filesystem::path& path)
		{
			const auto iterator = path.begin();
			if (iterator == path.end())
			{
				return false;
			}

			std::wstring firstComponent = iterator->wstring();
			std::transform(firstComponent.begin(), firstComponent.end(), firstComponent.begin(), ::towlower);
			return firstComponent == L"assets";
		}

		std::filesystem::path ResolveFromAssetsRoot(const std::filesystem::path& path)
		{
			std::filesystem::path resolvedPath = ASSETS_FOLDER;

			bool isFirstComponent = true;
			for (const auto& component : path)
			{
				if (isFirstComponent)
				{
					isFirstComponent = false;
					continue;
				}

				resolvedPath /= component;
			}

			return resolvedPath.lexically_normal();
		}
	}

	const wchar_t* MeshRegistry::GetRegistryName() const
	{
		return L"MeshRegistry";
	}

	std::filesystem::path MeshRegistry::ResolveSourcePath(const std::filesystem::path& path) const
	{
		if (path.is_absolute())
		{
			return path.lexically_normal();
		}

		if (IsAssetsRootRelative(path))
		{
			return ResolveFromAssetsRoot(path);
		}

		std::filesystem::path resolvedPath = MODELS_FOLDER;
		resolvedPath /= path;
		return resolvedPath.lexically_normal();
	}

	MeshAssetLocator MeshRegistry::MakeMetadataForRegisteredPath(const std::filesystem::path& resolvedSourcePath) const
	{
		MeshAssetLocator locator = {};
		locator.SourcePath = resolvedSourcePath;
		return locator;
	}

	std::filesystem::path MeshRegistry::GetSourcePathFromMetadata(const MeshAssetLocator& metadata) const
	{
		return metadata.SourcePath;
	}

	std::wstring MeshRegistry::GetCacheKeyFromMetadata(const MeshAssetLocator& metadata) const
	{
		return BuildMeshCacheKey(metadata);
	}

	const wchar_t* MeshRegistry::GetAssetTypeName() const
	{
		return L"mesh";
	}

	std::wstring MeshRegistry::BuildMeshCacheKey(const MeshAssetLocator& locator)
	{
		std::wstring cacheKey = BuildCacheKeyFromResolvedPath(locator.SourcePath);

		if (locator.HasSubAssetIndex())
		{
			cacheKey += L"#mesh:";
			cacheKey += std::to_wstring(locator.SubAssetIndex);
		}

		return cacheKey;
	}
	
	MeshHandle MeshRegistry::Register(const MeshAssetLocator& locator)
	{
		if (locator.SourcePath.empty())
		{
			WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Failed to register mesh: empty source path.\n");
			return {};
		}

		MeshAssetLocator resolvedLocator = locator;
		resolvedLocator.SourcePath = ResolveSourcePath(locator.SourcePath);

		const std::wstring cacheKey = BuildMeshCacheKey(resolvedLocator);
		const RegistrationResult registrationResult = RegisterResolvedPath(resolvedLocator.SourcePath, cacheKey);

		return FinalizeRegistration(registrationResult, std::move(resolvedLocator));
	}

	MeshHandle MeshRegistry::FindHandle(const MeshAssetLocator& locator) const
	{
		if (locator.SourcePath.empty())
		{
			return {};
		}

		MeshAssetLocator resolvedLocator = locator;
		resolvedLocator.SourcePath = ResolveSourcePath(locator.SourcePath);

		const std::uint32_t handleValue = FindHandleValueByKey(BuildMeshCacheKey(resolvedLocator));
		if (handleValue == InvalidHandleValue)
		{
			return {};
		}

		return MakeHandleFromValue(handleValue, L"find mesh handle by locator");
	}

	const Mesh* MeshRegistry::GetMesh(const MeshHandle handle) const
	{
		return GetAsset(handle);
	}

	const Mesh* MeshRegistry::FindMesh(const std::filesystem::path& path) const
	{
		return GetMesh(TypedAssetRegistry<Mesh, MeshHandle, MeshAssetLocator>::FindHandle(path));
	}

	const Mesh* MeshRegistry::FindMesh(const MeshAssetLocator& locator) const
	{
		return GetMesh(FindHandle(locator));
	}

	const Mesh* MeshRegistry::Load(const std::filesystem::path& path)
	{
		MeshHandle loadedHandle = {};
		return Load(path, loadedHandle);
	}

	const Mesh* MeshRegistry::Load(const std::filesystem::path& path, MeshHandle& outHandle)
	{
		MeshAssetLocator locator = {};
		locator.SourcePath = path;
		return Load(locator, outHandle);
	}

	const Mesh* MeshRegistry::Load(const MeshAssetLocator& locator)
	{
		MeshHandle loadedHandle;
		return Load(locator, loadedHandle);
	}

	const Mesh* MeshRegistry::Load(const MeshAssetLocator& locator, MeshHandle& outHandle)
	{
		outHandle = {};

		if (locator.SourcePath.empty())
		{
			WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Failed to load mesh: empty source path.\n");
			return nullptr;
		}

		const MeshHandle meshHandle = Register(locator);
		if (!meshHandle.IsValid())
		{
			return nullptr;
		}

		const MeshAssetLocator resolvedLocator = GetLocator(meshHandle);
		if (resolvedLocator.SourcePath.empty())
		{
			WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Failed to load mesh: could not resolve locator.\n");
			return nullptr;
		}

		const Mesh* cachedMesh = GetMesh(meshHandle);
		if (cachedMesh != nullptr)
		{
			if (resolvedLocator.HasSubAssetIndex())
			{
				WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Reusing cached mesh sub-asset " +
					std::to_wstring(resolvedLocator.SubAssetIndex) + L" from path: " + resolvedLocator.SourcePath.generic_wstring() + L"\n");
			}
			else
			{
				WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Reusing cached mesh for path: " +
					resolvedLocator.SourcePath.generic_wstring() + L"\n");
			}

			outHandle = meshHandle;
			return cachedMesh;
		}

		std::unique_ptr<Mesh> importedMesh;
		if (resolvedLocator.HasSubAssetIndex())
		{
			WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Importing mesh sub-asset " +
				std::to_wstring(resolvedLocator.SubAssetIndex) + L" from path: " + resolvedLocator.SourcePath.generic_wstring() + L"\n");

			importedMesh = MeshImporter::ImportMeshSubAsset(resolvedLocator.SourcePath, resolvedLocator.SubAssetIndex);
		}
		else
		{
			WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Importing mesh from path: " +
				resolvedLocator.SourcePath.generic_wstring() + L"\n");

			importedMesh = MeshImporter::ImportSingleMeshAsset(resolvedLocator.SourcePath);
		}

		if (importedMesh == nullptr)
		{
			if (resolvedLocator.HasSubAssetIndex())
			{
				WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Failed to import mesh sub-asset " +
					std::to_wstring(resolvedLocator.SubAssetIndex) + L" from path: " +
					resolvedLocator.SourcePath.generic_wstring() + L"\n");
			}
			else
			{
				WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Failed to import mesh from path: " +
					resolvedLocator.SourcePath.generic_wstring() + L"\n");
			}

			return nullptr;
		}

		if (resolvedLocator.HasSubAssetIndex())
		{
			WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Imported mesh sub-asset " +
				std::to_wstring(resolvedLocator.SubAssetIndex) + L" with " + std::to_wstring(importedMesh->GetSubMeshCount()) +
				L" submeshes from path: " + resolvedLocator.SourcePath.generic_wstring() + L"\n");
		}
		else
		{
			WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Imported mesh with " +
				std::to_wstring(importedMesh->GetSubMeshCount()) + L" submeshes from path: " +
				resolvedLocator.SourcePath.generic_wstring() + L"\n");
		}

		outHandle = meshHandle;
		return TypedAssetRegistry<Mesh, MeshHandle, MeshAssetLocator>::Cache(meshHandle, std::move(importedMesh));
	}

	MeshAssetLocator MeshRegistry::GetLocator(const MeshHandle handle) const
	{
		const auto* record = TryGetRecord(handle, L"get locator");
		if (record == nullptr)
		{
			return {};
		}

		return record->Metadata;
	}
}
