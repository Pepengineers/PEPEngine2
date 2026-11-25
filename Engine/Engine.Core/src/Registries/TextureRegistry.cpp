// TextureRegistry.cpp

#include <Engine.Core/Registries/TextureRegistry.h>
#include <Engine.Core/IO/TextureLoader.h>

namespace Engine::Core
{
	const wchar_t* TextureRegistry::GetRegistryName() const
	{
		return L"TextureRegistry";
	}

	std::filesystem::path TextureRegistry::ResolveSourcePath(const std::filesystem::path& path) const
	{
		if (path.is_absolute())
		{
			return path.lexically_normal();
		}

		std::filesystem::path resolvedPath = TEXTURES_FOLDER;
		resolvedPath /= path;
		return resolvedPath.lexically_normal();
	}

	TextureSource TextureRegistry::MakeMetadataForRegisteredPath(const std::filesystem::path& resolvedSourcePath) const
	{
		return {resolvedSourcePath};
	}

	std::filesystem::path TextureRegistry::GetSourcePathFromMetadata(const TextureSource& metadata) const
	{
		return metadata.Path;
	}

	std::wstring TextureRegistry::GetCacheKeyFromMetadata(const TextureSource& metadata) const
	{
		auto key = BuildCacheKeyFromResolvedPath(metadata.Path);
		if (metadata.Type != ETextureType::Unknown)
			key += L"#type:" + std::to_wstring(static_cast<unsigned>(metadata.Type));
		return key;
	}

	TextureHandle TextureRegistry::Register(const std::filesystem::path& path, ETextureType type)
	{
		if (path.empty()) return {};
		TextureSource source{ResolveSourcePath(path), type};
		const auto registration = RegisterResolvedPath(source.Path, GetCacheKeyFromMetadata(source));
		return FinalizeRegistration(registration, std::move(source));
	}

	TextureHandle TextureRegistry::FindHandle(const std::filesystem::path& path, ETextureType type) const
	{
		if (path.empty()) return {};
		const auto value = FindHandleValueByKey(GetCacheKeyFromMetadata({ResolveSourcePath(path), type}));
		return value == InvalidHandleValue ? TextureHandle{} : MakeHandleFromValue(value, L"find texture handle");
	}

	const wchar_t* TextureRegistry::GetAssetTypeName() const
	{
		return L"texture";
	}

	const Texture* TextureRegistry::GetTexture(const TextureHandle handle) const
	{
		return GetAsset(handle);
	}

	const Texture* TextureRegistry::FindTexture(const std::filesystem::path& path) const
	{
		return GetTexture(FindHandle(path));
	}

	const Texture* TextureRegistry::Load(const std::filesystem::path& path, ETextureType type)
	{
		TextureHandle loadedHandle = {};
		return Load(path, loadedHandle, type);
	}

	const Texture* TextureRegistry::Load(const std::filesystem::path& path, TextureHandle& outHandle, ETextureType type)
	{
		outHandle = {};

		if (path.empty())
		{
			WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Failed to load texture: empty path.\n");
			return nullptr;
		}

		const TextureHandle textureHandle = Register(path, type);
		if (!textureHandle.IsValid())
		{
			return nullptr;
		}

		const std::filesystem::path sourcePath = GetSourcePath(textureHandle);
		if (sourcePath.empty())
		{
			WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Failed to load texture: could not resolve source path.\n");
			return nullptr;
		}

		const Texture* cachedTexture = GetTexture(textureHandle);
		if (cachedTexture != nullptr)
		{
			WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Reusing cached texture for path: " + sourcePath.generic_wstring() + L"\n");

			outHandle = textureHandle;
			return cachedTexture;
		}

		WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Loading texture from path: " + sourcePath.generic_wstring() + L"\n");

		std::unique_ptr<Texture> loadedTexture = TextureLoader::LoadTextureAsset(sourcePath, false, type);
		if (loadedTexture == nullptr)
		{
			WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Failed to load texture from path: " + sourcePath.generic_wstring() + L"\n");
			return nullptr;
		}

		WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Loaded texture with " +
			std::to_wstring(loadedTexture->GetSubresourceCount()) + L" subresources from path: " +
			sourcePath.generic_wstring() + L"\n");

		outHandle = textureHandle;
		return Cache(textureHandle, std::move(loadedTexture));
	}

	const Texture* TextureRegistry::LoadOrDefault(const std::filesystem::path& path)
	{
		TextureHandle loadedHandle = {};
		return LoadOrDefault(path, loadedHandle);
	}

	const Texture* TextureRegistry::LoadOrDefault(const std::filesystem::path& path, TextureHandle& outHandle)
	{
		const Texture* loadedTexture = Load(path, outHandle);
		if (loadedTexture != nullptr)
		{
			return loadedTexture;
		}

		WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Falling back to default texture for path: " +
			ResolveSourcePath(path).generic_wstring() + L"\n");

		const Texture* defaultTexture = GetDefaultTexture();
		if (defaultTexture == nullptr)
		{
			outHandle = {};
			return nullptr;
		}

		if (_defaultTextureHandle.IsValid())
		{
			outHandle = _defaultTextureHandle;
		}
		else
		{
			outHandle = {};
		}

		return defaultTexture;
	}

	const Texture* TextureRegistry::GetDefaultTexture()
	{
		if (_defaultTextureHandle.IsValid())
		{
			const Texture* defaultTexture = GetTexture(_defaultTextureHandle);
			if (defaultTexture != nullptr)
			{
				return defaultTexture;
			}

			_defaultTextureHandle = {};
		}

		if (_fallbackDefaultTexture != nullptr)
		{
			return _fallbackDefaultTexture.get();
		}

		const std::filesystem::path defaultTexturePath = L"missing_texture.dds";
		TextureHandle defaultTextureHandle = {};
		const Texture* loadedDefaultTexture = Load(defaultTexturePath, defaultTextureHandle);
		if (loadedDefaultTexture != nullptr)
		{
			_defaultTextureHandle = defaultTextureHandle;
			if (_defaultTextureHandle.IsValid())
			{
				WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Loaded default texture from path: " +
					GetSourcePath(_defaultTextureHandle).generic_wstring() + L"\n");
			}

			return loadedDefaultTexture;
		}

		_fallbackDefaultTexture = TextureLoader::CreateDefaultTexture();
		if (_fallbackDefaultTexture == nullptr)
		{
			WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Failed to create fallback default texture.\n");
			return nullptr;
		}

		WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Using built-in fallback default texture.\n");
		return _fallbackDefaultTexture.get();
	}
}
