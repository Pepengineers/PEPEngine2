// AssetRegistryBase.cpp

#include <Engine.Core/Registries/AssetRegistryBase.h>

#include <cwctype>
#include <cassert>
#include <Windows.h>

namespace
{
	void WriteLogLine(const std::wstring& message)
	{
		OutputDebugStringW(message.c_str());
	}
}

namespace Engine::Core
{
	void AssetRegistryBase::WriteRegistryLog(const std::wstring& message)
	{
		WriteLogLine(message);
	}
	
	std::wstring AssetRegistryBase::BuildCacheKey(const std::filesystem::path& path) const
	{
		return BuildCacheKeyFromResolvedPath(ResolveSourcePath(path));
	}

	std::wstring AssetRegistryBase::BuildCacheKeyFromResolvedPath(const std::filesystem::path& resolvedSourcePath)
	{
		// generic_wstring - converts the path to a string with a unified slash format (with '/' instead of '\')
		// (e.g. "models\\cube.fbx" -> "models/cube.fbx")
		std::wstring cacheKey = resolvedSourcePath.generic_wstring();

		std::transform(cacheKey.begin(), cacheKey.end(), cacheKey.begin(), [](const wchar_t character) { return static_cast<wchar_t>(std::towlower(character)); });

		return cacheKey;
	}

	AssetRegistryBase::RegistrationResult AssetRegistryBase::RegisterResolvedPath(const std::filesystem::path& sourcePath, const std::wstring& cacheKey)
	{
		RegistrationResult registrationResult;
		registrationResult.SourcePath = sourcePath;
		registrationResult.CacheKey = cacheKey;

		if (sourcePath.empty() || cacheKey.empty())
		{
			WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Failed to register asset: empty source path or cache key.\n");
			return registrationResult;
		}

		const auto handleIterator = _handlesByPath.find(cacheKey);
		if (handleIterator != _handlesByPath.end())
		{
			registrationResult.HandleValue = handleIterator->second;
			registrationResult.bAlreadyRegistered = true;

			WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Reusing handle " + std::to_wstring(registrationResult.HandleValue) +
				L" for key: " + registrationResult.CacheKey + L"\n");

			return registrationResult;
		}

		registrationResult.HandleValue = _nextHandleValue++;
		_handlesByPath.emplace(registrationResult.CacheKey, registrationResult.HandleValue);

		WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Registered handle " +
			std::to_wstring(registrationResult.HandleValue) + L" for key: " + registrationResult.CacheKey + L"\n");

		return registrationResult;
	}

	AssetRegistryBase::RegistrationResult AssetRegistryBase::RegisterPath(const std::filesystem::path& path)
	{
		if (path.empty())
		{
			WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Failed to register path: empty path.\n");
			return {};
		}

		const std::filesystem::path resolvedSourcePath = ResolveSourcePath(path);
		const std::wstring cacheKey = BuildCacheKeyFromResolvedPath(resolvedSourcePath);
		return RegisterResolvedPath(resolvedSourcePath, cacheKey);
	}

	bool AssetRegistryBase::IsRegisteredKey(const std::wstring& cacheKey) const
	{
		if (cacheKey.empty())
		{
			return false;
		}

		return _handlesByPath.find(cacheKey) != _handlesByPath.end();
	}

	std::uint32_t AssetRegistryBase::FindHandleValueByKey(const std::wstring& cacheKey) const
	{
		if (cacheKey.empty())
		{
			return InvalidHandleValue;
		}

		const auto handleIterator = _handlesByPath.find(cacheKey);
		if (handleIterator == _handlesByPath.end())
		{
			return InvalidHandleValue;
		}

		return handleIterator->second;
	}

	void AssetRegistryBase::ClearRegistry()
	{
		_handlesByPath.clear();
		_nextHandleValue = 0;
	}

	bool AssetRegistryBase::IsRegistered(const std::filesystem::path& path) const
	{
		if (path.empty())
		{
			WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Failed to check registration: empty path.\n");
			return false;
		}

		return IsRegisteredKey(BuildCacheKey(path));
	}

	std::uint32_t AssetRegistryBase::FindHandleValue(const std::filesystem::path& path) const
	{
		if (path.empty())
		{
			WriteRegistryLog(L"[" + std::wstring(GetRegistryName()) + L"] Failed to find handle: empty path.\n");
			return InvalidHandleValue;
		}

		return FindHandleValueByKey(BuildCacheKey(path));
	}

	size_t AssetRegistryBase::GetRegisteredCount() const
	{
		return _handlesByPath.size();
	}
}