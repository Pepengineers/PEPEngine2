#include "Engine.Core/ConsoleModule.h"

#include <algorithm>
#include <cwctype>

namespace
{
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

        std::vector<IConsoleVariable*> GetConsoleVariables() const
        {
            std::scoped_lock lock(Mutex);

            std::vector<IConsoleVariable*> result;
            result.reserve(Variables.size());

            for (const auto& [key, variable] : Variables)
            {
                result.push_back(variable);
            }

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


IConsoleVariable::IConsoleVariable(const std::wstring_view Name, const std::wstring_view Description) : Name(Name)
    , Description(Description)
{
    const bool wasRegistered = ConsoleVariableStorage::Get().RegisterConsoleVariable(this);
    assert(wasRegistered);
    (void)wasRegistered;
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

std::vector<IConsoleVariable*> ConsoleModule::GetConsoleVariables() const
{
    return ConsoleVariableStorage::Get().GetConsoleVariables();
}
