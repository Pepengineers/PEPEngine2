#include "Engine.Core/ConsoleModule.h"
#include "Engine.Core/Logging/Logger.h"
#include "Common/RuntimePaths.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <unordered_set>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <ini.h>

namespace
{
    std::atomic<bool> StartupComplete{ false };
    std::mutex ConfigMutex;

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
            if (inserted) ++Revision;
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
            ++Revision;
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

        std::vector<IConsoleVariable*> GetConsoleVariables(std::wstring_view prefix, uint64_t* revision) const
        {
            std::scoped_lock lock(Mutex);
            if (revision) *revision = Revision;
            if (prefix.empty())
            {
                if (SortedRevision != Revision)
                {
                    SortedVariables.clear();
                    SortedVariables.reserve(Variables.size());
                    for (const auto& [key, variable] : Variables) SortedVariables.push_back(variable);
                    std::sort(SortedVariables.begin(), SortedVariables.end(), [](const auto* left, const auto* right)
                        { return left->GetName() < right->GetName(); });
                    SortedRevision = Revision;
                }
                return SortedVariables;
            }

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

        uint64_t GetRevision() const
        {
            std::scoped_lock lock(Mutex);
            return Revision;
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
        uint64_t Revision = 0;
        mutable uint64_t SortedRevision = UINT64_MAX;
        mutable std::vector<IConsoleVariable*> SortedVariables;
    };
    template<class OnVariable>
    ConsoleConfigResult ParseIni(const std::filesystem::path& path, bool optional, OnVariable onVariable)
    {
        FILE* rawFile = nullptr;
        const auto openError = _wfopen_s(&rawFile, path.c_str(), L"rb");
        if (openError == ENOENT && optional)
        {
            Log::Info("Config", "INI not found; keeping C++ defaults: ", path);
            return {};
        }
        if (openError != 0) return {0, "Cannot open INI: " + std::generic_category().message(openError)};
        std::unique_ptr<FILE, decltype(&std::fclose)> file(rawFile, &std::fclose);
        struct Context
        {
            OnVariable& OnEntry;
            std::string Error;
        } state{onVariable, {}};
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
            context.Error = context.OnEntry(std::move(wideName), value);
            return context.Error.empty();
        };
        const int result = ini_parse_file(file.get(), handler, &state);
        if (std::ferror(file.get())) return {0, "Cannot read INI"};
        if (result == 0) return {};
        if (state.Error.empty()) state.Error = result > 0 ? "Invalid INI syntax" : "Cannot read INI";
        return {result > 0 ? result : 0, std::move(state.Error)};
    }

    struct ResolvedValue
    {
        IConsoleVariable* Variable;
        std::string_view Value;
    };

    ConsoleConfigResult ResolvePatch(const ConsoleModule& console, const ConsoleVariablePatch& patch,
        bool checkReadOnly, std::vector<ResolvedValue>& resolved)
    {
        std::unordered_set<IConsoleVariable*> seen;
        resolved.reserve(patch.size());
        for (const auto& [name, value] : patch)
        {
            IConsoleVariable* variable = nullptr;
            const auto index = static_cast<int>(resolved.size() + 1);
            const auto key = std::filesystem::path(name).u8string();
            if (!console.TryFindConsoleVariable(name, variable)) return {index, "Unknown CVar: " + key};
            if (!seen.insert(variable).second) return {index, "Duplicate CVar: " + key};
            if (checkReadOnly && StartupComplete.load() && variable->GetFlags() == EConsoleVariableFlags::ReadOnly)
                return {index, "Startup-only CVar: " + key};
            if (!variable->IsValidString(value)) return {index, "Invalid CVar value: " + key};
            resolved.push_back({variable, value});
        }
        return {};
    }
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

bool IConsoleVariable::SaveConfigValue(std::string_view value) const
{
    if (!StartupComplete.load()) return true;
    std::scoped_lock lock(ConfigMutex);
    const auto path = ConsoleModule::GetDefaultConfigPath();
    const auto fail = [&](std::string_view reason)
    {
        Log::Error("Config", "Cannot save ", Name, " to ", path, ": ", reason);
        return false;
    };
    std::error_code error;
    std::string contents;
    if (std::filesystem::exists(path, error))
    {
        std::ifstream input(path, std::ios::binary);
        if (!input) return fail("cannot open INI");
        contents.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
        if (input.bad()) return fail("cannot read INI");
    }
    if (error) return fail(error.message());

    const auto name = std::filesystem::path(Name).u8string();
    bool replaced = false;
    bool hasSection = false;
    const auto trim = [](std::string_view text)
    {
        const auto begin = text.find_first_not_of(" \t\r");
        return begin == std::string_view::npos ? std::string_view{} :
            text.substr(begin, text.find_last_not_of(" \t\r") - begin + 1);
    };
    for (size_t start = 0; start < contents.size();)
    {
        const auto end = contents.find('\n', start);
        const auto length = (end == std::string::npos ? contents.size() : end) - start;
        auto line = std::string_view(contents.data() + start, length);
        if (start == 0 && line.substr(0, 3) == "\xEF\xBB\xBF") line.remove_prefix(3);
        const auto trimmed = trim(line);
        if (trimmed.substr(0, 18) == "[ConsoleVariables]") hasSection = true;
        const auto equals = line.find('=');
        if (equals != std::string_view::npos)
        {
            const auto key = trim(line.substr(0, equals));
            if (!key.empty() && key.front() != ';' && key.front() != '#' &&
                _wcsicmp(std::filesystem::u8path(key).c_str(), Name.c_str()) == 0)
            {
                // Update every duplicate: the loader uses the last occurrence.
                const auto comment = line.find(';', equals + 1);
                const auto valueEnd = comment == std::string_view::npos ? line.size() : comment;
                const auto oldValue = line.substr(equals + 1, valueEnd - equals - 1);
                const auto first = oldValue.find_first_not_of(" \t\r");
                const auto last = oldValue.find_last_not_of(" \t\r");
                const auto offset = first == std::string_view::npos ? 0 : first;
                contents.replace(static_cast<size_t>(line.data() - contents.data()) + equals + 1 + offset,
                    first == std::string_view::npos ? 0 : last - first + 1, value);
                replaced = true;
            }
        }
        const auto next = contents.find('\n', start);
        if (next == std::string::npos) break;
        start = next + 1;
    }
    if (!replaced)
    {
        const auto newline = contents.find("\r\n") == std::string::npos ? "\n" : "\r\n";
        if (!contents.empty() && contents.back() != '\n') contents += newline;
        if (!hasSection) contents += std::string("[ConsoleVariables]") + newline;
        contents += name + " = " + std::string(value) + newline;
    }

    std::filesystem::create_directories(path.parent_path(), error);
    if (error) return fail(error.message());
    auto temporary = path;
    temporary += L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        output.close();
        if (!output)
        {
            std::filesystem::remove(temporary, error);
            return fail("cannot write temporary INI");
        }
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        const auto message = std::system_category().message(GetLastError());
        std::filesystem::remove(temporary, error);
        return fail(message);
    }
    return true;
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
    return ParseIni(Path, true, [this](std::wstring name, const char* value) -> std::string
    {
        IConsoleVariable* variable = nullptr;
        if (!TryFindConsoleVariable(name, variable))
            return "Unknown CVar: " + std::filesystem::path(name).u8string();
        if (!variable->SetFromString(value))
            return "Invalid value or read-only CVar: " + std::filesystem::path(name).u8string();
        return {};
    });
}

ConsoleConfigResult ConsoleModule::ReadPatchIni(const std::filesystem::path& Path, ConsoleVariablePatch& Out)
{
    ConsoleVariablePatch patch;
    auto result = ParseIni(Path, false, [&](std::wstring name, const char* value) -> std::string
    {
        patch.emplace_back(std::move(name), value);
        return {};
    });
    if (!result.Error.empty()) return result;
    if (patch.empty()) return {0, "Empty CVar overlay"};
    Out = std::move(patch);
    return {};
}

ConsoleConfigResult ConsoleModule::ApplyPatch(const ConsoleVariablePatch& Patch, ConsoleVariablePatch* Previous) const
{
    std::vector<ResolvedValue> resolved;
    if (auto result = ResolvePatch(*this, Patch, true, resolved); !result.Error.empty()) return result;

    ConsoleVariablePatch snapshot;
    snapshot.reserve(resolved.size());
    for (const auto& [variable, value] : resolved)
        snapshot.emplace_back(variable->GetName(), variable->GetString());

    for (size_t index = 0; index < resolved.size(); ++index)
    {
        if (resolved[index].Variable->SetTransientFromString(resolved[index].Value)) continue;
        for (size_t applied = 0; applied < index; ++applied)
            if (!resolved[applied].Variable->SetTransientFromString(snapshot[applied].second))
                throw std::logic_error("Failed to restore CVar patch at a quiescent frame boundary");
        return {static_cast<int>(index + 1), "CVar changed during patch application"};
    }
    if (Previous) *Previous = std::move(snapshot);
    return {};
}

ConsoleConfigResult ConsoleModule::SavePatchIni(const std::filesystem::path& Path,
    const ConsoleVariablePatch& Patch) const
{
    std::vector<ResolvedValue> resolved;
    if (auto result = ResolvePatch(*this, Patch, false, resolved); !result.Error.empty()) return result;
    if (resolved.empty()) return {0, "Empty CVar profile"};
    std::scoped_lock lock(ConfigMutex);
    std::error_code error;
    if (std::filesystem::equivalent(Path, GetDefaultConfigPath(), error))
        return {0, "Cannot replace the base INI with a profile"};
    error.clear();
    if (Path.has_parent_path()) std::filesystem::create_directories(Path.parent_path(), error);
    if (error) return {0, "Cannot create profile directory: " + error.message()};
    auto temporary = Path;
    temporary += L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
    const auto fail = [&](std::string message)
    {
        std::filesystem::remove(temporary, error);
        return ConsoleConfigResult{0, std::move(message)};
    };
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output << "[ConsoleVariables]\n";
        for (const auto& [variable, value] : resolved)
            output << std::filesystem::path(variable->GetName()).u8string() << " = " << value << '\n';
        output.close();
        if (!output) return fail("Cannot write temporary CVar profile");
    }
    if (std::filesystem::exists(Path, error))
    {
        auto backup = Path;
        backup += L".bak";
        std::filesystem::copy_file(Path, backup, std::filesystem::copy_options::overwrite_existing, error);
    }
    if (error) return fail("Cannot back up CVar profile: " + error.message());
    if (!MoveFileExW(temporary.c_str(), Path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return fail("Cannot replace CVar profile: " + std::system_category().message(GetLastError()));
    return {};
}

void ConsoleModule::Initialize()
{
    InitializeWithOverlay({});
}

void ConsoleModule::InitializeWithOverlay(const std::filesystem::path& Overlay)
{
    if (StartupComplete.load())
    {
        if (!Overlay.empty()) throw ConsoleConfigError("The startup overlay must be applied before initialization");
        return;
    }
    auto path = GetDefaultConfigPath();
    Log::Info("Config", "Loading INI: ", path);
    auto result = LoadIni(path);
    if (result.Error.empty() && !Overlay.empty())
    {
        path = Overlay;
        ConsoleVariablePatch patch;
        result = ReadPatchIni(path, patch);
        if (result.Error.empty()) result = ApplyPatch(patch);
    }
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

std::vector<IConsoleVariable*> ConsoleModule::GetConsoleVariables(std::wstring_view Prefix, uint64_t* Revision) const
{
    return ConsoleVariableStorage::Get().GetConsoleVariables(Prefix, Revision);
}

uint64_t ConsoleModule::GetConsoleVariableRevision() const
{
    return ConsoleVariableStorage::Get().GetRevision();
}
