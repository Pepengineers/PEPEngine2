#include "Engine.Core/ConsoleModule.h"
#include "Engine.Core/Logging/Logger.h"
#include "Common/RuntimePaths.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <system_error>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <ini.h>

namespace
{
    std::atomic<bool> StartupComplete{ false };

    class ConsoleVariableStorage final
    {
    public:
        bool RegisterConsoleVariable(IConsoleVariable* variable)
        {
            if (variable == nullptr)
            {
                return false;
            }

            const auto variableName = variable->GetName();
            if (variableName.empty())
            {
                return false;
            }

            std::scoped_lock lock(Mutex);
            auto [it, inserted] = Variables.emplace(MakeLookupKey(variableName), variable);
            return inserted || it->second == variable;
        }

        bool UnregisterConsoleVariable(const IConsoleVariable* variable)
        {
            if (variable == nullptr)
            {
                return false;
            }

            const auto variableName = variable->GetName();
            if (variableName.empty())
            {
                return false;
            }

            std::scoped_lock lock(Mutex);
            const auto foundIt = Variables.find(MakeLookupKey(variableName));
            if (foundIt == Variables.end() || foundIt->second != variable)
            {
                return false;
            }

            Variables.erase(foundIt);
            return true;
        }

        bool TryFindConsoleVariable(std::wstring_view Name, IConsoleVariable*& OutVariable)
        {
            std::scoped_lock lock(Mutex);
            const auto foundIt = Variables.find(MakeLookupKey(Name));
            if (foundIt == Variables.end())
            {
                OutVariable = nullptr;
                return false;
            }

            OutVariable = foundIt->second;
            return true;
        }

        std::vector<IConsoleVariable*> GetConsoleVariables(std::wstring_view prefix) const
        {
            std::scoped_lock lock(Mutex);

            std::vector<IConsoleVariable*> result;
            result.reserve(Variables.size());
            const auto lookupPrefix = MakeLookupKey(prefix);

            for (const auto& [key, variable] : Variables)
            {
                if (key.compare(0, lookupPrefix.size(), lookupPrefix) == 0) result.push_back(variable);
            }

            std::sort(result.begin(), result.end(), [](const auto* left, const auto* right)
            {
                return left->GetName() < right->GetName();
            });

            return result;
        }

        static ConsoleVariableStorage& Get()
        {
            static ConsoleVariableStorage instance;
            return instance;
        }

    private:
        using ConsoleVariableMap = std::unordered_map<std::wstring, IConsoleVariable*>;

        static std::wstring MakeLookupKey(std::wstring_view Name)
        {
            std::wstring key(Name);
            std::transform(key.begin(), key.end(), key.begin(), [](const wchar_t character)
            {
                return static_cast<wchar_t>(std::towlower(character));
            });

            return key;
        }

        ConsoleVariableStorage() = default;

        mutable std::mutex Mutex;
        ConsoleVariableMap Variables;
    };
}


IConsoleVariable::IConsoleVariable(const std::wstring_view Name, const std::wstring_view Description,
    EConsoleVariableFlags Flags) : Name(Name)
    , Description(Description)
    , Flags(Flags)
{
    if (!ConsoleVariableStorage::Get().RegisterConsoleVariable(this))
        throw std::invalid_argument("Invalid or duplicate CVar name: " + std::filesystem::path(std::wstring(Name)).u8string());
}

bool IConsoleVariable::CanSetValue() const
{
    return Flags != EConsoleVariableFlags::ReadOnly || !StartupComplete.load();
}

IConsoleVariable::~IConsoleVariable()
{
    ConsoleVariableStorage::Get().UnregisterConsoleVariable(this);
}

std::wstring_view IConsoleVariable::GetName() const
{
    return Name;
}

std::wstring_view IConsoleVariable::GetDescription() const
{
    return Description;
}

std::filesystem::path ConsoleModule::GetDefaultConfigPath()
{
    return Engine::Core::GetExecutableDirectory() / L"Config" / L"Engine.ini";
}

ConsoleConfigResult ConsoleModule::LoadIni(const std::filesystem::path& Path)
{
    if (StartupComplete.load()) return { 0, "INI configuration can only be loaded during startup" };

    FILE* rawFile = nullptr;
    const auto openError = _wfopen_s(&rawFile, Path.c_str(), L"rb");
    if (openError == ENOENT)
    {
        Log::Info("Config", "INI not found; keeping C++ defaults: ", Path);
        return {};
    }
    if (openError != 0) return { 0, "Cannot open INI: " + std::generic_category().message(openError) };
    std::unique_ptr<FILE, decltype(&std::fclose)> file(rawFile, &std::fclose);
    struct Context
    {
        ConsoleModule& Console;
        std::string Error;
    } context{ *this, {} };
    const auto handler = [](void* user, const char* section, const char* name, const char* value) -> int
    {
        auto& context = *static_cast<Context*>(user);
        if (std::strcmp(section, "ConsoleVariables") != 0)
        {
            context.Error = "Expected section [ConsoleVariables]";
            return 0;
        }
        if (!name) return 1;
        const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, nullptr, 0);
        if (length == 0)
        {
            context.Error = "CVar name is not valid UTF-8";
            return 0;
        }
        std::wstring wideName(length, L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, wideName.data(), length);
        wideName.pop_back();
        IConsoleVariable* variable = nullptr;
        if (!context.Console.TryFindConsoleVariable(wideName, variable))
        {
            context.Error = std::string("Unknown CVar: ") + name;
            return 0;
        }
        if (!variable->SetFromString(value))
        {
            context.Error = std::string("Invalid value or read-only CVar: ") + name;
            return 0;
        }
        return 1;
    };
    const int result = ini_parse_file(file.get(), handler, &context);
    if (std::ferror(file.get())) return { 0, "Cannot read INI" };
    if (result == 0) return {};
    if (context.Error.empty()) context.Error = result > 0 ? "Invalid INI syntax" : "Cannot read INI";
    return { result > 0 ? result : 0, std::move(context.Error) };
}

void ConsoleModule::Initialize()
{
    if (StartupComplete.load()) return;
    const auto path = GetDefaultConfigPath();
    Log::Info("Config", "Loading INI: ", path);
    const auto result = LoadIni(path);
    if (!result.Error.empty())
    {
        auto message = path.u8string() + ":" + std::to_string(result.ErrorLine) + ": " + result.Error +
            ". Application startup aborted.\n";
        if (!Log::Error("Config", message) || !Log::Flush())
        {
            message += "Could not write startup log: " +
                (path.parent_path().parent_path() / L"Logs" / L"Engine.log").u8string() + '\n';
            std::cerr << message;
            OutputDebugStringA(message.c_str());
        }
        throw ConsoleConfigError(message);
    }
    StartupComplete.store(true);
    Log::Info("Config", "Startup configuration applied; ReadOnly variables are locked");
}

bool ConsoleModule::RegisterConsoleVariable(IConsoleVariable* variable)
{
    return ConsoleVariableStorage::Get().RegisterConsoleVariable(variable);
}

bool ConsoleModule::UnregisterConsoleVariable(const IConsoleVariable* variable)
{
    return ConsoleVariableStorage::Get().UnregisterConsoleVariable(variable);
}

bool ConsoleModule::TryFindConsoleVariable(std::wstring_view Name, IConsoleVariable*& OutVariable) const
{
    return ConsoleVariableStorage::Get().TryFindConsoleVariable(Name, OutVariable);
}

IConsoleVariable& ConsoleModule::FindConsoleVariable(std::wstring_view Name) const
{
    IConsoleVariable* variable = nullptr;
    if (!TryFindConsoleVariable(Name, variable))
        throw std::out_of_range("Required CVar not found: " + std::filesystem::path(std::wstring(Name)).u8string());
    return *variable;
}

std::vector<IConsoleVariable*> ConsoleModule::GetConsoleVariables(std::wstring_view Prefix) const
{
    return ConsoleVariableStorage::Get().GetConsoleVariables(Prefix);
}
