// SceneRegistry.cpp

#include <Engine.Core/Registries/SceneRegistry.h>
#include <Engine.Core/Registries/MeshRegistry.h>

namespace Engine::Core
{
	const wchar_t* SceneRegistry::GetRegistryName() const
	{
		return L"SceneRegistry";
	}

	std::filesystem::path SceneRegistry::ResolveSourcePath(const std::filesystem::path& path) const
	{
		if (path.is_absolute())
		{
			return path.lexically_normal();
		}

		std::filesystem::path resolvedPath = MODELS_FOLDER;
		resolvedPath /= path;
		return resolvedPath.lexically_normal();
	}

	SceneHandle SceneRegistry::Register(const std::filesystem::path& path)
	{
		const RegistrationResult registrationResult = RegisterPath(path);
		if (registrationResult.HandleValue == InvalidHandleValue)
		{
			return {};
		}

		if (!registrationResult.bAlreadyRegistered)
		{
			assert(_sceneAssets.size() == static_cast<size_t>(registrationResult.HandleValue));

			Record record;
			record.SourcePath = registrationResult.SourcePath;

			_sceneAssets.push_back(std::move(record));
		}

		SceneHandle sceneHandle;
		sceneHandle.Value = registrationResult.HandleValue;
		return sceneHandle;
	}

	SceneHandle SceneRegistry::FindHandle(const std::filesystem::path& path) const
	{
		const std::uint32_t handleValue = FindHandleValue(path);
		if (handleValue == InvalidHandleValue)
		{
			return {};
		}

		SceneHandle sceneHandle;
		sceneHandle.Value = handleValue;
		return sceneHandle;
	}

	std::shared_ptr<const SceneAsset> SceneRegistry::GetScene(const SceneHandle handle) const
	{
		if (!handle.IsValid())
		{
			WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Failed to get scene: invalid handle.\n");
			return nullptr;
		}

		const size_t sceneIndex = static_cast<size_t>(handle.Value);
		if (sceneIndex >= _sceneAssets.size())
		{
			WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Failed to get scene: handle index out of range: " + std::to_wstring(handle.Value) + L"\n");
			return nullptr;
		}

		return _sceneAssets[sceneIndex].SceneData;
	}

	std::shared_ptr<const SceneAsset> SceneRegistry::FindScene(const std::filesystem::path& path) const
	{
		return GetScene(FindHandle(path));
	}

	std::shared_ptr<const SceneAsset> SceneRegistry::Cache(const SceneHandle handle, std::shared_ptr<SceneAsset> data)
	{
		assert(handle.IsValid());
		assert(data != nullptr);

		if (!handle.IsValid() || data == nullptr)
		{
			WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Failed to cache scene: invalid handle or null scene data.\n");
			return nullptr;
		}

		const size_t sceneIndex = static_cast<size_t>(handle.Value);
		if (sceneIndex >= _sceneAssets.size())
		{
			WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Failed to cache scene: handle index out of range: " + std::to_wstring(handle.Value) + L"\n");
			return nullptr;
		}

		Record& record = _sceneAssets[sceneIndex];
		if (record.SceneData != nullptr)
		{
			WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Scene data already cached for handle " + std::to_wstring(handle.Value) + L".\n");
			return record.SceneData;
		}

		record.SceneData = std::move(data);

		WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Cached scene data for handle " + std::to_wstring(handle.Value) + L". Path: " + record.SourcePath.generic_wstring() + L"\n");
		return record.SceneData;
	}

	std::shared_ptr<const SceneAsset> SceneRegistry::Cache(const std::filesystem::path& path, std::shared_ptr<SceneAsset> data)
	{
		const SceneHandle sceneHandle = Register(path);
		if (!sceneHandle.IsValid())
		{
			return nullptr;
		}

		return Cache(sceneHandle, std::move(data));
	}

	std::filesystem::path SceneRegistry::GetSourcePath(const SceneHandle handle) const
	{
		if (!handle.IsValid())
		{
			WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Failed to get source path to the scene handle " + std::to_wstring(handle.Value) + L": invalid handle.\n");
			return {};
		}

		const size_t sceneIndex = static_cast<size_t>(handle.Value);
		if (sceneIndex >= _sceneAssets.size())
		{
			WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Failed to get source path to the scene handle: handle index out of range: " + std::to_wstring(handle.Value) + L"\n");
			return {};
		}

		return _sceneAssets[sceneIndex].SourcePath;
	}

	bool SceneRegistry::IsLoaded(const std::filesystem::path& path) const
	{
		const SceneHandle sceneHandle = FindHandle(path);
		if (!sceneHandle.IsValid())
		{
			return false;
		}

		return GetScene(sceneHandle) != nullptr;
	}

	size_t SceneRegistry::GetLoadedCount() const
	{
		size_t loadedCount = 0;

		for (const Record& record : _sceneAssets)
		{
			if (record.SceneData != nullptr)
			{
				++loadedCount;
			}
		}

		return loadedCount;
	}

	void SceneRegistry::Unload(const SceneHandle handle)
	{
		if (!handle.IsValid())
		{
			WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Failed to unload scene: invalid handle.\n");
			return;
		}

		const size_t sceneIndex = static_cast<size_t>(handle.Value);
		if (sceneIndex >= _sceneAssets.size())
		{
			WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Failed to unload scene: handle index out of range: " + std::to_wstring(handle.Value) + L"\n");
			return;
		}

		Record& record = _sceneAssets[sceneIndex];
		if (record.SceneData == nullptr)
		{
			WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Scene handle " + std::to_wstring(handle.Value) + L" has no cached data to unload.\n");
			return;
		}

		record.SceneData.reset();

		WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Unloaded scene data for handle " + std::to_wstring(handle.Value) + L".\n");
	}

	void SceneRegistry::UnloadAll()
	{
		_sceneAssets.clear();
		ClearRegistry();

		WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Cleared all scene registry entries.\n");
	}
}