// TextureRegistry.cpp

#include <Engine.Core/Registries/TextureRegistry.h>

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

	TextureHandle TextureRegistry::Register(const std::filesystem::path& path)
	{
		const RegistrationResult registrationResult = RegisterPath(path);
		if (registrationResult.HandleValue == InvalidHandleValue)
		{
			return {};
		}

		if (!registrationResult.bAlreadyRegistered)
		{
			assert(_textureAssets.size() == static_cast<size_t>(registrationResult.HandleValue));

			Record record;
			record.SourcePath = registrationResult.SourcePath;

			_textureAssets.push_back(std::move(record));
		}

		TextureHandle textureHandle;
		textureHandle.Value = registrationResult.HandleValue;
		return textureHandle;
	}

	TextureHandle TextureRegistry::FindHandle(const std::filesystem::path& path) const
	{
		const std::uint32_t handleValue = FindHandleValue(path);
		if (handleValue == InvalidHandleValue)
		{
			return {};
		}

		TextureHandle textureHandle;
		textureHandle.Value = handleValue;
		return textureHandle;
	}

	std::shared_ptr<const Texture> TextureRegistry::GetTexture(const TextureHandle handle) const
	{
		if (!handle.IsValid())
		{
			WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Failed to get texture: invalid handle.\n");
			return nullptr;
		}

		const size_t textureIndex = static_cast<size_t>(handle.Value);
		if (textureIndex >= _textureAssets.size())
		{
			WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Failed to get texture: handle index out of range: " + std::to_wstring(handle.Value) + L"\n");
			return nullptr;
		}

		return _textureAssets[textureIndex].TextureData;
	}

	std::shared_ptr<const Texture> TextureRegistry::FindTexture(const std::filesystem::path& path) const
	{
		return GetTexture(FindHandle(path));
	}

	std::shared_ptr<const Texture> TextureRegistry::Cache(const TextureHandle handle, std::shared_ptr<Texture> data)
	{
		assert(handle.IsValid());
		assert(data != nullptr);

		if (!handle.IsValid() || data == nullptr)
		{
			WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Failed to cache texture: invalid handle or null texture data.\n");
			return nullptr;
		}

		const size_t textureIndex = static_cast<size_t>(handle.Value);
		if (textureIndex >= _textureAssets.size())
		{
			WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Failed to cache texture: handle index out of range: " + std::to_wstring(handle.Value) + L"\n");
			return nullptr;
		}

		Record& record = _textureAssets[textureIndex];
		if (record.TextureData != nullptr)
		{
			WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Texture data already cached for handle " + std::to_wstring(handle.Value) + L".\n");
			return record.TextureData;
		}

		record.TextureData = std::move(data);

		WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Cached texture data for handle " + std::to_wstring(handle.Value) + L". Path: " + record.SourcePath.generic_wstring() + L"\n");
		
		return record.TextureData;
	}

	std::shared_ptr<const Texture> TextureRegistry::Cache(const std::filesystem::path& path, std::shared_ptr<Texture> data)
	{
		const TextureHandle textureHandle = Register(path);
		if (!textureHandle.IsValid())
		{
			return nullptr;
		}

		return Cache(textureHandle, std::move(data));
	}

	std::filesystem::path TextureRegistry::GetSourcePath(const TextureHandle handle) const
	{
		if (!handle.IsValid())
		{
			WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Failed to get source path to the texture handle " + std::to_wstring(handle.Value) + L": invalid handle.\n");
			return {};
		}

		const size_t textureIndex = static_cast<size_t>(handle.Value);
		if (textureIndex >= _textureAssets.size())
		{
			WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Failed to get source path to the texture handle: handle index out of range: " + std::to_wstring(handle.Value) + L"\n");
			return {};
		}

		return _textureAssets[textureIndex].SourcePath;
	}

	bool TextureRegistry::IsLoaded(const std::filesystem::path& path) const
	{
		const TextureHandle textureHandle = FindHandle(path);
		if (!textureHandle.IsValid())
		{
			return false;
		}

		return GetTexture(textureHandle) != nullptr;
	}

	size_t TextureRegistry::GetLoadedCount() const
	{
		size_t loadedCount = 0;

		for (const Record& record : _textureAssets)
		{
			if (record.TextureData != nullptr)
			{
				++loadedCount;
			}
		}

		return loadedCount;
	}

	void TextureRegistry::Unload(const TextureHandle handle)
	{
		if (!handle.IsValid())
		{
			WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Failed to unload texture: invalid handle.\n");
			return;
		}

		const size_t textureIndex = static_cast<size_t>(handle.Value);
		if (textureIndex >= _textureAssets.size())
		{
			WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Failed to unload texture: handle index out of range: " + std::to_wstring(handle.Value) + L"\n");
			return;
		}

		Record& record = _textureAssets[textureIndex];
		if (record.TextureData == nullptr)
		{
			WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Texture handle " + std::to_wstring(handle.Value) + L" has no cached data to unload.\n");
			return;
		}

		record.TextureData.reset();

		WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Unloaded texture data for handle " + std::to_wstring(handle.Value) + L".\n");
	}

	void TextureRegistry::UnloadAll()
	{
		_textureAssets.clear();
		ClearRegistry();

		WriteRegistryLog(L"[TextureRegistry] Cleared all texture registry entries.\n");
	}
}