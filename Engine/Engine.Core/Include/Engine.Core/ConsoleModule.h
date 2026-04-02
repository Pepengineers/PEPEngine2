#pragma once

#include <cassert>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "Common/Module.h"

class IConsoleVariable
{
public:
    IConsoleVariable(std::wstring_view Name, std::wstring_view Description = {});

    virtual ~IConsoleVariable();

    IConsoleVariable(const IConsoleVariable&) = delete;
    IConsoleVariable& operator=(const IConsoleVariable&) = delete;
    IConsoleVariable(IConsoleVariable&&) = delete;
    IConsoleVariable& operator=(IConsoleVariable&&) = delete;

    std::wstring_view GetName() const;

    std::wstring_view GetDescription() const;

    virtual bool GetBool() const = 0;
    virtual int GetInt() const = 0;
    virtual float GetFloat() const = 0;
    virtual uint32_t GetUInt() const = 0;

private:
    std::wstring Name;
    std::wstring Description;
};

class ConsoleModule final : public Module
{
public:
    void Initialize() override
    {
    };

    void Uninitialize() override
    {
    };

    bool RegisterConsoleVariable(IConsoleVariable* variable);
    bool UnregisterConsoleVariable(const IConsoleVariable* variable);
    bool TryFindConsoleVariable(std::wstring_view Name, IConsoleVariable*& OutVariable) const;
    std::vector<IConsoleVariable*> GetConsoleVariables() const;
};
