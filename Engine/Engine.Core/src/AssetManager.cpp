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

#pragma region Accessors
	MeshRegistry& AssetManager::Meshes()
	{
		return _meshRegistry;
	}

	const MeshRegistry& AssetManager::Meshes() const
	{
		return _meshRegistry;
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