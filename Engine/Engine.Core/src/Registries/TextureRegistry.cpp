// TextureRegistry.cpp

#include <Engine.Core/Registries/TextureRegistry.h>
#include <Engine.Core/IO/TextureLoader.h>

#include <cassert>
#include <windows.h>

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

	std::filesystem::path TextureRegistry::MakeMetadataForRegisteredPath(const std::filesystem::path& resolvedSourcePath) const
	{
		return resolvedSourcePath;
	}

	std::filesystem::path TextureRegistry::GetSourcePathFromMetadata(const std::filesystem::path& metadata) const
	{
		return metadata;
	}

	const wchar_t* TextureRegistry::GetAssetTypeName() const
	{
		return L"texture";
	}

	std::shared_ptr<const Texture> TextureRegistry::GetTexture(const TextureHandle handle) const
	{
		return GetAsset(handle);
	}

	std::shared_ptr<const Texture> TextureRegistry::FindTexture(const std::filesystem::path& path) const
	{
		return GetTexture(FindHandle(path));
	}

	std::shared_ptr<const Texture> TextureRegistry::Load(const std::filesystem::path& path)
	{
		if (path.empty())
		{
			WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Failed to load texture: empty path.\n");
			return nullptr;
		}

		const std::shared_ptr<const Texture> cachedTexture = FindTexture(path);
		if (cachedTexture != nullptr)
		{
			WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Reusing cached texture for path: " +
				ResolveSourcePath(path).generic_wstring() + L"\n");

			return cachedTexture;
		}

		const TextureHandle textureHandle = Register(path);
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

		WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Loading texture from path: " +
			sourcePath.generic_wstring() + L"\n");

		std::shared_ptr<Texture> loadedTexture = TextureLoader::LoadTextureAsset(sourcePath);
		if (loadedTexture == nullptr)
		{
			WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Failed to load texture from path: " +
				sourcePath.generic_wstring() + L"\n");

			return nullptr;
		}

		WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Loaded texture with " +
			std::to_wstring(loadedTexture->GetSubresourceCount()) + L" subresources from path: " +
			sourcePath.generic_wstring() + L"\n");

		return Cache(textureHandle, std::move(loadedTexture));
	}
}