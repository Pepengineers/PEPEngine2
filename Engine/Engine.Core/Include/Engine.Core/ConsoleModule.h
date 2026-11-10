#pragma once

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Common/Module.h"

enum class EConsoleVariableFlags : std::uint8_t
{
    Default = 0,
    ReadOnly = 1
};

enum class EConsoleVariableType : std::uint8_t
{
    Boolean,
    Integer,
    UnsignedInteger,
    FloatingPoint,
    Enumeration
};

struct ConsoleConfigResult
{
    int ErrorLine = 0;
    std::string Error;
};

using ConsoleVariablePatch = std::vector<std::pair<std::wstring, std::string>>;

class ConsoleConfigError final : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

class IConsoleVariable
{
public:
    IConsoleVariable(std::wstring_view Name, std::wstring_view Description = {},
        EConsoleVariableFlags Flags = EConsoleVariableFlags::Default);

    virtual ~IConsoleVariable();

    IConsoleVariable(const IConsoleVariable&) = delete;
    IConsoleVariable& operator=(const IConsoleVariable&) = delete;
    IConsoleVariable(IConsoleVariable&&) = delete;
    IConsoleVariable& operator=(IConsoleVariable&&) = delete;

    std::wstring_view GetName() const;

    std::wstring_view GetDescription() const;
    EConsoleVariableFlags GetFlags() const { return Flags; }
    virtual EConsoleVariableType GetType() const = 0;
    virtual int GetEnumCount() const = 0;
    virtual int GetEnumFirstValue() const = 0;
    virtual std::string_view GetEnumValueName(int Value) const = 0;
    virtual bool SetFromString(std::string_view Value) = 0;
    virtual bool IsValidString(std::string_view Value) const = 0;
    virtual std::string GetString() const = 0;
    // Settings edits may save ReadOnly values for the next startup.
    virtual bool SetConfigFromString(std::string_view Value) = 0;
    virtual std::string GetConfigString() const = 0;

    virtual bool GetBool() const = 0;
    virtual int GetInt() const = 0;
    virtual float GetFloat() const = 0;
    virtual uint32_t GetUInt() const = 0;

protected:
    bool CanSetValue() const;
    bool SaveConfigValue(std::string_view Value) const;

private:
    friend class ConsoleModule;
    virtual bool SetTransientFromString(std::string_view Value) = 0;
    std::wstring Name;
    std::wstring Description;
    EConsoleVariableFlags Flags;
};

class ConsoleModule final : public Module
{
public:
    void Initialize() override;
    void InitializeWithOverlay(const std::filesystem::path& Overlay, const std::filesystem::path& Overrides = {});

    static std::filesystem::path GetDefaultConfigPath();
    ConsoleConfigResult LoadIni(const std::filesystem::path& Path);
    static ConsoleConfigResult ReadPatchIni(const std::filesystem::path& Path, ConsoleVariablePatch& Out);
    // Call between frames, with jobs quiescent. ReadOnly settings can only be patched during startup.
    ConsoleConfigResult ApplyPatch(const ConsoleVariablePatch& Patch, ConsoleVariablePatch* Previous = nullptr) const;
    ConsoleConfigResult SavePatchIni(const std::filesystem::path& Path, const ConsoleVariablePatch& Patch) const;

    void Uninitialize() override
    {
    };

    bool RegisterConsoleVariable(IConsoleVariable* variable);
    bool UnregisterConsoleVariable(const IConsoleVariable* variable);
    bool TryFindConsoleVariable(std::wstring_view Name, IConsoleVariable*& OutVariable) const;
    IConsoleVariable& FindConsoleVariable(std::wstring_view Name) const;
    // Sorted snapshot and optional revision are captured together; owners must outlive iteration. Prefix ignores case.
    std::vector<IConsoleVariable*> GetConsoleVariables(std::wstring_view Prefix = {}, uint64_t* Revision = nullptr) const;
    uint64_t GetConsoleVariableRevision() const;
};
