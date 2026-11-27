#pragma once
#include "Common/ConsoleVariables.h"

#if defined(_DEBUG) || defined(DEBUG)
template<class T>
using DebugConsoleVariable = AutoConsoleVariable<T>;
#else
// Release uses the authored default without constructing or registering a CVar.
template<class T>
class DebugConsoleVariable
{
public:
    constexpr DebugConsoleVariable(std::wstring_view, T value, std::wstring_view = {},
        EConsoleVariableFlags = EConsoleVariableFlags::Default, typename ConsoleVariableBase<T>::Validator = nullptr)
        : _value(value) {}

    constexpr T GetValue() const { return _value; }

private:
    const T _value;
};
#endif
