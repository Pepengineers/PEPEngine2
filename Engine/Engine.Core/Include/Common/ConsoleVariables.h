#pragma once
#include "Engine.Core/ConsoleModule.h"

#include <charconv>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace ConsoleVariableDetail
{
    template <class T>
    bool ParseValue(std::string_view text, T& value)
    {
        if (text.empty()) return false;
        if constexpr (std::is_enum_v<T>)
        {
            std::underlying_type_t<T> parsed{};
            if (!ParseValue(text, parsed)) return false;
            value = static_cast<T>(parsed);
            return true;
        }
        else if constexpr (std::is_same_v<T, bool>)
        {
            if (text == "true" || text == "1") value = true;
            else if (text == "false" || text == "0") value = false;
            else return false;
            return true;
        }
        else
        {
            using Parsed = std::conditional_t<std::is_integral_v<T>,
                std::conditional_t<std::is_signed_v<T>, long long, unsigned long long>, T>;
            Parsed parsed{};
            const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
            if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) return false;
            if constexpr (std::is_integral_v<T>)
            {
                if (parsed < (std::numeric_limits<T>::lowest)() || parsed > (std::numeric_limits<T>::max)())
                    return false;
            }
            else if (!std::isfinite(parsed)) return false;
            value = static_cast<T>(parsed);
            return true;
        }
    }
}

template <class TValue>
class ConsoleVariableBase : public IConsoleVariable
{
    static_assert(std::is_arithmetic_v<TValue> || std::is_enum_v<TValue>,
        "Console variable supports only arithmetic and enum value types.");

public:
    using Validator = bool (*)(TValue);

    ConsoleVariableBase(std::wstring_view Name, std::wstring_view Description,
        EConsoleVariableFlags Flags, Validator Validate)
        : IConsoleVariable(Name, Description, Flags), Validate(Validate) {}

    TValue GetValue() const
    {
        std::scoped_lock lock(ValueMutex);
        return GetTypedValue();
    }

    bool SetValue(TValue value)
    {
        std::scoped_lock lock(ValueMutex);
        if (!CanSetValue() || !IsValidValue(value)) return false;
        SetTypedValue(value);
        return true;
    }

    bool SetFromString(std::string_view text) override
    {
        TValue value{};
        return ConsoleVariableDetail::ParseValue(text, value) && SetValue(value);
    }

    bool GetBool() const override { return static_cast<bool>(GetValue()); }
    int GetInt() const override { return static_cast<int>(GetValue()); }
    float GetFloat() const override { return static_cast<float>(GetValue()); }
    uint32_t GetUInt() const override { return static_cast<uint32_t>(GetValue()); }

protected:
    bool IsValidValue(TValue value) const
    {
        if constexpr (std::is_floating_point_v<TValue>)
            if (!std::isfinite(value)) return false;
        return !Validate || Validate(value);
    }

    virtual TValue GetTypedValue() const = 0;
    virtual void SetTypedValue(TValue Value) = 0;

private:
    Validator Validate;
    mutable std::mutex ValueMutex;
};

template <class TValue>
class AutoConsoleVariable final : public ConsoleVariableBase<TValue>
{
    using Super = ConsoleVariableBase<TValue>;

public:
    AutoConsoleVariable(std::wstring_view Name, TValue DefaultValue, std::wstring_view Description = {},
        EConsoleVariableFlags Flags = EConsoleVariableFlags::Default, typename Super::Validator Validate = nullptr)
        : Super(Name, Description, Flags, Validate), Value(DefaultValue)
    {
        if (!this->IsValidValue(Value)) throw std::invalid_argument("Invalid default CVar value");
    }

protected:
    TValue GetTypedValue() const override { return Value; }
    void SetTypedValue(TValue value) override { Value = value; }

private:
    TValue Value;
};

template <class TValue>
class AutoConsoleVariableRef final : public ConsoleVariableBase<TValue>
{
    using Super = ConsoleVariableBase<TValue>;

public:
    AutoConsoleVariableRef(std::wstring_view Name, TValue& ValueRef, std::wstring_view Description = {},
        EConsoleVariableFlags Flags = EConsoleVariableFlags::Default, typename Super::Validator Validate = nullptr)
        : Super(Name, Description, Flags, Validate), RefValue(ValueRef)
    {
        if (!this->IsValidValue(RefValue)) throw std::invalid_argument("Invalid default CVar value");
    }

protected:
    TValue GetTypedValue() const override { return RefValue; }
    void SetTypedValue(TValue value) override { RefValue = value; }

private:
    TValue& RefValue;
};
