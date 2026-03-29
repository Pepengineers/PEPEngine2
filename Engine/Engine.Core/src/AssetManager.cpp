// AssetManager.cpp

#include <Engine.Core/AssetManager.h>

#include <cassert>
#include <cwctype>

namespace
{
	void LogAssetManagerMessage(const std::wstring& message)
	{
		OutputDebugStringW(message.c_str());
	}

#define LOG_ASSET_MANAGER(message) \
	LogAssetManagerMessage(L"[" + std::wstring(__FILEW__) + L":" + std::to_wstring(__LINE__) + L"] [" + std::wstring(__FUNCTIONW__) + L"] " + (message) + L"\n")
}

namespace Engine::Core
{
#pragma region Internal Helpers
	std::filesystem::path AssetManager::ResolveMeshSourcePath(const std::filesystem::path& meshPath)
	{
		if (meshPath.is_absolute())
		{
			return meshPath.lexically_normal();
		}

		std::filesystem::path resolvedMeshPath = MODELS_FOLDER;
		resolvedMeshPath /= meshPath;
		return resolvedMeshPath.lexically_normal();
	}

	std::filesystem::path AssetManager::ResolveTextureSourcePath(const std::filesystem::path& texturePath)
	{
		if (texturePath.is_absolute())
		{
			return texturePath.lexically_normal();
		}

		std::filesystem::path resolvedTexturePath = TEXTURES_FOLDER;
		resolvedTexturePath /= texturePath;
		return resolvedTexturePath.lexically_normal();
	}
	
	std::wstring AssetManager::BuildMeshCacheKey(const std::filesystem::path& meshPath)
	{
		const std::filesystem::path resolvedMeshPath = ResolveMeshSourcePath(meshPath);
		// generic_wstring - converts the path to a string with a unified slash format (with '/' instead of '\')
		// (e.g. "models\\cube.fbx" -> "models/cube.fbx")
		std::wstring cacheKey = resolvedMeshPath.generic_wstring();

		std::transform(cacheKey.begin(), cacheKey.end(), cacheKey.begin(), [](const wchar_t character) { return static_cast<wchar_t>(std::towlower(character)); });
		return cacheKey;
	}

	std::wstring AssetManager::BuildTextureCacheKey(const std::filesystem::path& texturePath)
	{
		const std::filesystem::path resolvedTexturePath = ResolveTextureSourcePath(texturePath);
		std::wstring cacheKey = resolvedTexturePath.generic_wstring();

		std::transform(
			cacheKey.begin(),
			cacheKey.end(),
			cacheKey.begin(),
			[](const wchar_t character)
			{
				return static_cast<wchar_t>(std::towlower(character));
			});

		return cacheKey;
	}
#pragma endregion Internal Helpers

#pragma region Singleton
	// Meyers Singleton
	AssetManager& AssetManager::GetInstance()
	{
		static AssetManager instance;
		return instance;
	}
#pragma endregion Singleton

#pragma region Mesh Registry
	MeshHandle AssetManager::RegisterMesh(const std::filesystem::path& meshPath)
	{
		if (meshPath.empty())
		{
			LOG_ASSET_MANAGER(L"Failed to register mesh: empty path.\n");
			return {};
		}

		const std::wstring cacheKey = BuildMeshCacheKey(meshPath);
		const auto meshHandleIterator = _meshHandlesByPath.find(cacheKey);
		if (meshHandleIterator != _meshHandlesByPath.end())
		{
			const std::filesystem::path resolvedMeshPath = ResolveMeshSourcePath(meshPath);
			LOG_ASSET_MANAGER(L"Mesh already registered, reusing handle for path: " + resolvedMeshPath.generic_wstring() + L"\n");
			return meshHandleIterator->second;
		}

		if (_meshAssets.size() >= static_cast<size_t>(MeshHandle::InvalidValue))
		{
			LOG_ASSET_MANAGER(L"Failed to register mesh: registry is full. Path: " + ResolveMeshSourcePath(meshPath).generic_wstring());
			assert(false);
			return {};
		}

		MeshHandle meshHandle;
		meshHandle.Value = static_cast<std::uint32_t>(_meshAssets.size());

		MeshAssetRecord meshAssetRecord;
		meshAssetRecord.SourcePath = ResolveMeshSourcePath(meshPath);

		_meshAssets.push_back(std::move(meshAssetRecord));
		_meshHandlesByPath.emplace(std::move(cacheKey), meshHandle);

		LOG_ASSET_MANAGER(L"Registered mesh handle " + std::to_wstring(meshHandle.Value) + L" for path: " + meshAssetRecord.SourcePath.generic_wstring() + L"\n");
		
		return meshHandle;
	}

	bool AssetManager::IsMeshRegistered(const std::filesystem::path& meshPath) const
	{
		if (meshPath.empty())
		{
			return false;
		}

		return _meshHandlesByPath.find(BuildMeshCacheKey(meshPath)) != _meshHandlesByPath.end();
	}

	MeshHandle AssetManager::FindMeshHandle(const std::filesystem::path& meshPath) const
	{
		if (meshPath.empty())
		{
			LOG_ASSET_MANAGER(L"Failed to find mesh handle: empty path.");
			return {};
		}

		const auto meshHandleIterator = _meshHandlesByPath.find(BuildMeshCacheKey(meshPath));
		if (meshHandleIterator == _meshHandlesByPath.end())
		{
			LOG_ASSET_MANAGER(L"Mesh handle not found for path: " + ResolveMeshSourcePath(meshPath).generic_wstring());
			return {};
		}

		return meshHandleIterator->second;
	}

	std::shared_ptr<const Mesh> AssetManager::GetMesh(const MeshHandle meshHandle) const
	{
		if (!meshHandle.IsValid())
		{
			LOG_ASSET_MANAGER(L"Failed to get mesh: invalid handle.");
			return nullptr;
		}

		const size_t meshIndex = static_cast<size_t>(meshHandle.Value);
		if (meshIndex >= _meshAssets.size())
		{
			LOG_ASSET_MANAGER(L"Failed to get mesh: handle index out of range: " + std::to_wstring(meshHandle.Value));
			return nullptr;
		}

		return _meshAssets[meshIndex].MeshData;
	}

	std::shared_ptr<const Mesh> AssetManager::CacheMesh(const MeshHandle meshHandle, std::shared_ptr<Mesh> meshData)
	{
		assert(meshHandle.IsValid());
		assert(meshData != nullptr);

		if (!meshHandle.IsValid() || meshData == nullptr)
		{
			LOG_ASSET_MANAGER(L"Failed to cache mesh: invalid handle or null data.");
			return nullptr;
		}

		const size_t meshIndex = static_cast<size_t>(meshHandle.Value);
		if (meshIndex >= _meshAssets.size())
		{
			LOG_ASSET_MANAGER(L"Failed to cache mesh: handle index out of range: " + std::to_wstring(meshHandle.Value));
			return nullptr;
		}

		MeshAssetRecord& meshAssetRecord = _meshAssets[meshIndex];
		if (meshAssetRecord.MeshData != nullptr)
		{
			LOG_ASSET_MANAGER(L"Mesh slot already has data, returning existing for handle: " + std::to_wstring(meshHandle.Value));
			return meshAssetRecord.MeshData;
		}

		meshAssetRecord.MeshData = std::move(meshData);
		
		LOG_ASSET_MANAGER(L"Cached mesh data for handle: " + std::to_wstring(meshHandle.Value));
		
		return meshAssetRecord.MeshData;
	}

	bool AssetManager::IsMeshLoaded(const std::filesystem::path& meshPath) const
	{
		const MeshHandle meshHandle = FindMeshHandle(meshPath);
		if (!meshHandle.IsValid())
		{
			return false;
		}

		return GetMesh(meshHandle) != nullptr;
	}

	std::shared_ptr<const Mesh> AssetManager::FindMesh(const std::filesystem::path& meshPath) const
	{
		return GetMesh(FindMeshHandle(meshPath));
	}

	std::shared_ptr<const Mesh> AssetManager::CacheMesh(const std::filesystem::path& meshPath, std::shared_ptr<Mesh> meshData)
	{
		const MeshHandle meshHandle = RegisterMesh(meshPath);
		if (!meshHandle.IsValid())
		{
			return nullptr;
		}

		return CacheMesh(meshHandle, std::move(meshData));
	}

	size_t AssetManager::GetRegisteredMeshCount() const
	{
		return _meshAssets.size();
	}

	size_t AssetManager::GetLoadedMeshCount() const
	{
		size_t loadedMeshCount = 0;

		for (const MeshAssetRecord& meshAssetRecord : _meshAssets)
		{
			if (meshAssetRecord.MeshData != nullptr)
			{
				++loadedMeshCount;
			}
		}

		return loadedMeshCount;
	}
#pragma endregion Mesh Registry

#pragma region Texture Registry
	TextureHandle AssetManager::RegisterTexture(const std::filesystem::path& texturePath)
	{
		if (texturePath.empty())
		{
			LOG_ASSET_MANAGER(L"Failed to register texture: empty path.");
			return {};
		}

		const std::wstring cacheKey = BuildTextureCacheKey(texturePath);
		const auto textureHandleIterator = _textureHandlesByPath.find(cacheKey);
		if (textureHandleIterator != _textureHandlesByPath.end())
		{
			LOG_ASSET_MANAGER(L"Texture already registered, reusing handle for path: " + ResolveTextureSourcePath(texturePath).generic_wstring());
			return textureHandleIterator->second;
		}

		if (_textureAssets.size() >= static_cast<size_t>(TextureHandle::InvalidValue))
		{
			LOG_ASSET_MANAGER(L"Failed to register texture: registry is full. Path: " + ResolveTextureSourcePath(texturePath).generic_wstring());
			assert(false);
			return {};
		}

		TextureHandle textureHandle;
		textureHandle.Value = static_cast<std::uint32_t>(_textureAssets.size());

		TextureAssetRecord textureAssetRecord;
		textureAssetRecord.SourcePath = ResolveTextureSourcePath(texturePath);

		_textureAssets.push_back(std::move(textureAssetRecord));
		_textureHandlesByPath.emplace(std::move(cacheKey), textureHandle);

		LOG_ASSET_MANAGER(L"Registered texture handle " + std::to_wstring(textureHandle.Value) + L" for path: " + textureAssetRecord.SourcePath.generic_wstring());
		
		return textureHandle;
	}

	bool AssetManager::IsTextureRegistered(const std::filesystem::path& texturePath) const
	{
		if (texturePath.empty())
		{
			return false;
		}

		return _textureHandlesByPath.find(BuildTextureCacheKey(texturePath)) != _textureHandlesByPath.end();
	}

	TextureHandle AssetManager::FindTextureHandle(const std::filesystem::path& texturePath) const
	{
		if (texturePath.empty())
		{
			LOG_ASSET_MANAGER(L"Failed to find texture handle: empty path.");
			return {};
		}

		const auto textureHandleIterator = _textureHandlesByPath.find(BuildTextureCacheKey(texturePath));
		if (textureHandleIterator == _textureHandlesByPath.end())
		{
			LOG_ASSET_MANAGER(L"Texture handle not found for path: " + ResolveTextureSourcePath(texturePath).generic_wstring());
			return {};
		}

		return textureHandleIterator->second;
	}

	size_t AssetManager::GetRegisteredTextureCount() const
	{
		return _textureAssets.size();
	}
#pragma endregion Texture Registry
}