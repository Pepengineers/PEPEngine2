#pragma once
#include "Engine.Core/ConsoleModule.h"


template <class TValue>
class ConsoleVariableBase : public IConsoleVariable
{
    static_assert(std::is_arithmetic_v<TValue> || std::is_enum_v<TValue>,
                  "Console variable supports only arithmetic and enum value types.");
    static_assert(sizeof(TValue) != 0, "Not supported");
    
public:
    ConsoleVariableBase(const std::wstring_view& Name, const std::wstring_view& Description)
        : IConsoleVariable(Name, Description)
    {
    }

    bool GetBool() const override
    {
        const TValue value = GetTypedValue();
        return static_cast<bool>(value);
    }

    int GetInt() const override
    {
        const TValue value = GetTypedValue();
        return static_cast<int>(value);
    }

    float GetFloat() const override
    {
        const TValue value = GetTypedValue();
        return static_cast<float>(value);
    }

    uint32_t GetUInt() const override
    {
        const TValue value = GetTypedValue();
        return static_cast<uint32_t>(value);
    }

protected:
    virtual TValue GetTypedValue() const = 0;
};


template <typename TValue>
class AutoConsoleVariable final : public ConsoleVariableBase<TValue>
{
    using Super = ConsoleVariableBase<TValue>;

public:
    AutoConsoleVariable(const std::wstring_view Name, const TValue DefaultValue,
                        const std::wstring_view Description = {})
        : Super(Name, Description), Value(DefaultValue)
    {
    }

    const TValue& GetValue() const
    {
        return Value;
    }

    void SetValue(const TValue InValue)
    {
        Value = InValue;
    }

protected:
    TValue GetTypedValue() const override
    {
        return Value;
    }

private:
    TValue Value;
};


template <class TValue>
class AutoConsoleVariableRef final : public ConsoleVariableBase<TValue>
{
    using Super = ConsoleVariableBase<TValue>;

public:
    AutoConsoleVariableRef(const std::wstring_view Name, TValue& ValueRef, const std::wstring_view Description = {})
        : Super(Name, Description), RefValue(ValueRef)
    {
    }

    const TValue& GetValue() const
    {
        return RefValue;
    }

    void SetValue(const TValue Value) const
    {
        RefValue = Value;
    }

protected:
    TValue GetTypedValue() const override
    {
        return RefValue;
    }

private:
    TValue& RefValue;
};

