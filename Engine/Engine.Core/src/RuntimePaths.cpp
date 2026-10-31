#include "Common/RuntimePaths.h"
#include <string>
#include <system_error>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

std::filesystem::path Engine::Core::GetExecutableDirectory()
{
    std::wstring executable(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (length == 0 || length == executable.size())
        throw std::system_error(GetLastError(), std::system_category(), "Cannot locate the application executable");
    executable.resize(length);
    return std::filesystem::path(executable).parent_path();
}
