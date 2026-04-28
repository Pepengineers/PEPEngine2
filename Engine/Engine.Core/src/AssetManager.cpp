// AssetManager.cpp

#include <Engine.Core/AssetManager.h>

#include <cassert>
#include <cwctype>

/*
namespace
{
	void LogAssetManagerMessage(const std::wstring& message)
	{
		OutputDebugStringW(message.c_str());
	}
	
	#define LOG_ASSET_MANAGER(message) \
		LogAssetManagerMessage(L"[" + std::wstring(__FILEW__) + L":" + std::to_wstring(__LINE__) + L"] [" + std::wstring(__FUNCTIONW__) + L"] " + (message) + L"\n")
	}
}
*/

namespace Engine::Core
{
	// Meyers Singleton
	AssetManager& AssetManager::GetInstance()
	{
		static AssetManager instance;
		return instance;
	}

	const Mesh* AssetManager::LoadMesh(const std::filesystem::path& path)
	{
		return _meshRegistry.Load(path);
	}

	const SceneAsset* AssetManager::LoadScene(const std::filesystem::path& path)
	{
		return _sceneRegistry.Load(path, _meshRegistry);
	}

	const Texture* AssetManager::LoadTexture(const std::filesystem::path& path)
	{
		return _textureRegistry.Load(path);
	}

	const Texture* AssetManager::LoadTextureOrDefault(const std::filesystem::path& path)
	{
		return _textureRegistry.LoadOrDefault(path);
	}

#pragma region Accessors
	MeshRegistry& AssetManager::Meshes()
	{
		return _meshRegistry;
	}

	const MeshRegistry& AssetManager::Meshes() const
	{
		return _meshRegistry;
	}

	SceneRegistry& AssetManager::Scenes()
	{
		return _sceneRegistry;
	}

	const SceneRegistry& AssetManager::Scenes() const
	{
		return _sceneRegistry;
	}

	TextureRegistry& AssetManager::Textures()
	{
		return _textureRegistry;
	}

	const TextureRegistry& AssetManager::Textures() const
	{
		return _textureRegistry;
	}
#pragma endregion Accessors
}