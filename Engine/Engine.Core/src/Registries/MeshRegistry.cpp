// MeshRegistry.cpp

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

	std::wstring MeshRegistry::BuildMeshCacheKey(const MeshAssetLocator& locator) const
	{
		std::wstring cacheKey = BuildCacheKey(locator.SourcePath);

		if (locator.HasSubAssetIndex())
		{
			cacheKey += L"#mesh:";
			cacheKey += std::to_wstring(locator.SubAssetIndex);
		}

		return cacheKey;
	}
	
	MeshHandle MeshRegistry::Register(const std::filesystem::path& path)
	{
		MeshAssetLocator locator;
		locator.SourcePath = path;
		return Register(locator);
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
		if (registrationResult.HandleValue == InvalidHandleValue)
		{
			return {};
		}

		if (!registrationResult.bAlreadyRegistered)
		{
			assert(_meshAssets.size() == static_cast<size_t>(registrationResult.HandleValue));

			Record record;
			record.Locator = std::move(resolvedLocator);

			_meshAssets.push_back(std::move(record));
		}

		MeshHandle meshHandle;
		meshHandle.Value = registrationResult.HandleValue;
		return meshHandle;
	}

	MeshHandle MeshRegistry::FindHandle(const std::filesystem::path& path) const
	{
		MeshAssetLocator locator;
		locator.SourcePath = path;
		return FindHandle(locator);
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

    	MeshHandle meshHandle;
    	meshHandle.Value = handleValue;
    	return meshHandle;
    }

	std::shared_ptr<const Mesh> MeshRegistry::GetMesh(const MeshHandle handle) const
	{
		if (!handle.IsValid())
		{
			WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Failed to get mesh: invalid handle.\n");
			return nullptr;
		}

		const size_t meshIndex = static_cast<size_t>(handle.Value);
		if (meshIndex >= _meshAssets.size())
		{
			WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Failed to get mesh: handle index out of range: " + std::to_wstring(handle.Value) + L"\n");
			return nullptr;
		}

		return _meshAssets[meshIndex].MeshData;
	}

	std::shared_ptr<const Mesh> MeshRegistry::FindMesh(const std::filesystem::path& path) const
	{
		return GetMesh(FindHandle(path));
	}

	std::shared_ptr<const Mesh> MeshRegistry::FindMesh(const MeshAssetLocator& locator) const
	{
		return GetMesh(FindHandle(locator));
	}

	std::shared_ptr<const Mesh> MeshRegistry::Cache(const MeshHandle handle, std::shared_ptr<Mesh> data)
	{
		assert(handle.IsValid());
		assert(data != nullptr);

		if (!handle.IsValid() || data == nullptr)
		{
			WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Failed to cache mesh: invalid handle or null mesh data.\n");
			return nullptr;
		}

		const size_t meshIndex = static_cast<size_t>(handle.Value);
		if (meshIndex >= _meshAssets.size())
		{
			WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Failed to cache mesh: handle index out of range: " + std::to_wstring(handle.Value) + L"\n");
			return nullptr;
		}

		Record& record = _meshAssets[meshIndex];
		if (record.MeshData != nullptr)
		{
			WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Mesh data already cached for handle " + std::to_wstring(handle.Value) + L".\n");
			return record.MeshData;
		}

		record.MeshData = std::move(data);

		WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Cached mesh data for handle " + std::to_wstring(handle.Value) + L". Path: " + record.Locator.SourcePath.generic_wstring() + L"\n");

		return record.MeshData;
	}

	std::shared_ptr<const Mesh> MeshRegistry::Cache(const std::filesystem::path& path, std::shared_ptr<Mesh> data)
	{
		const MeshHandle meshHandle = Register(path);
		if (!meshHandle.IsValid())
		{
			return nullptr;
		}

		return Cache(meshHandle, std::move(data));
	}

	std::shared_ptr<const Mesh> MeshRegistry::Cache(const MeshAssetLocator& locator, std::shared_ptr<Mesh> data)
	{
		const MeshHandle meshHandle = Register(locator);
		if (!meshHandle.IsValid())
		{
			return nullptr;
		}

		return Cache(meshHandle, std::move(data));
	}

	std::filesystem::path MeshRegistry::GetSourcePath(MeshHandle handle) const
	{
		if (!handle.IsValid())
		{
			WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Failed to get source path to the mesh handle " + std::to_wstring(handle.Value) + L": invalid handle.\n");
			return {};
		}

		const size_t meshIndex = static_cast<size_t>(handle.Value);
		if (meshIndex >= _meshAssets.size())
		{
			WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Failed to get source path to the mesh handle: handle index out of range: " + std::to_wstring(handle.Value) + L"\n");
			return {};
		}

		return _meshAssets[meshIndex].Locator.SourcePath;
	}

	MeshAssetLocator MeshRegistry::GetLocator(const MeshHandle handle) const
	{
		if (!handle.IsValid())
		{
			WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Failed to get locator: invalid handle.\n");
			return {};
		}

		const size_t meshIndex = static_cast<size_t>(handle.Value);
		if (meshIndex >= _meshAssets.size())
		{
			WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Failed to get locator: handle index out of range: " + std::to_wstring(handle.Value) + L"\n");
			return {};
		}

		return _meshAssets[meshIndex].Locator;
	}

	bool MeshRegistry::IsLoaded(const std::filesystem::path& path) const
	{
		const MeshHandle meshHandle = FindHandle(path);
		if (!meshHandle.IsValid())
		{
			return false;
		}

		return GetMesh(meshHandle) != nullptr;
	}

	size_t MeshRegistry::GetLoadedCount() const
	{
		size_t loadedCount = 0;
		
		for (const Record& record : _meshAssets)
		{
			if (record.MeshData != nullptr)
			{
				++loadedCount;
			}
		}

		return loadedCount;
	}

	void MeshRegistry::Unload(MeshHandle handle)
	{
		if (!handle.IsValid())
		{
			WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Failed to unload mesh: invalid handle.\n");
			return;
		}

		const size_t meshIndex = static_cast<size_t>(handle.Value);
		if (meshIndex >= _meshAssets.size())
		{
			WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Failed to unload mesh: handle index out of range: " + std::to_wstring(handle.Value) + L"\n");
			return;
		}

		Record& record = _meshAssets[meshIndex];
		if (record.MeshData == nullptr)
		{
			WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Mesh handle " + std::to_wstring(handle.Value) + L" has no cached data to unload.\n");
			return;
		}

		record.MeshData.reset();

		WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Unloaded mesh data for handle " + std::to_wstring(handle.Value) + L".\n");
	}

	void MeshRegistry::UnloadAll()
	{
		_meshAssets.clear();
		ClearRegistry();

		WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Cleared all mesh registry entries.\n");
	}
}