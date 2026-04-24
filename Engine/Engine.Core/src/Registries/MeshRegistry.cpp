#include <Engine.Core/Registries/MeshRegistry.h>

namespace Engine::Core
{
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

		return MeshHandle(handleValue);
	}

	std::shared_ptr<const Mesh> MeshRegistry::GetMesh(const MeshHandle handle) const
	{
		return GetAsset(handle);
	}

	std::shared_ptr<const Mesh> MeshRegistry::FindMesh(const std::filesystem::path& path) const
	{
		return GetMesh(TypedAssetRegistry<Mesh, MeshHandle, MeshAssetLocator>::FindHandle(path));
	}

	std::shared_ptr<const Mesh> MeshRegistry::FindMesh(const MeshAssetLocator& locator) const
	{
		return GetMesh(FindHandle(locator));
	}

	std::shared_ptr<const Mesh> MeshRegistry::Cache(const MeshAssetLocator& locator, std::shared_ptr<Mesh> data)
	{
		const MeshHandle meshHandle = Register(locator);
		if (!meshHandle.IsValid())
		{
			return nullptr;
		}

		return TypedAssetRegistry<Mesh, MeshHandle, MeshAssetLocator>::Cache(meshHandle, std::move(data));
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