#pragma once
#include "Engine.Core/ConsoleModule.h"

#include <charconv>
#include <cmath>
#include <limits>
#include <optional>
#include <stdexcept>

namespace ConsoleVariableDetail
{
    template <class T, class = void>
    struct EnumFirst { static constexpr int Value = 0; };

    template <class T>
    struct EnumFirst<T, std::void_t<decltype(T::First)>>
    {
        static constexpr int Value = static_cast<int>(T::First);
    };

    template <class T, class = void>
    struct EnumSize { static constexpr int Count = 0; };

    template <class T>
    struct EnumSize<T, std::void_t<decltype(T::Count)>>
    {
        static_assert(std::is_enum_v<T>, "Count is reserved for enum console variables");
        static_assert(static_cast<std::underlying_type_t<T>>(T::Count) > 0 &&
            static_cast<std::underlying_type_t<T>>(T::Count) <= (std::numeric_limits<int>::max)(),
            "Console enum must end with positive Count fitting in int");
        static constexpr int Count = static_cast<int>(T::Count) - EnumFirst<T>::Value;
        static_assert(Count > 0, "Console enum First must precede Count");
    };

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

    EConsoleVariableType GetType() const override
    {
        if constexpr (std::is_same_v<TValue, bool>) return EConsoleVariableType::Boolean;
        else if constexpr (std::is_enum_v<TValue>) return EConsoleVariableType::Enumeration;
        else if constexpr (std::is_floating_point_v<TValue>) return EConsoleVariableType::FloatingPoint;
        else if constexpr (std::is_unsigned_v<TValue>) return EConsoleVariableType::UnsignedInteger;
        else return EConsoleVariableType::Integer;
    }

    int GetEnumCount() const override { return ConsoleVariableDetail::EnumSize<TValue>::Count; }
    int GetEnumFirstValue() const override { return ConsoleVariableDetail::EnumFirst<TValue>::Value; }

    TValue GetValue() const
    {
        std::scoped_lock lock(ValueMutex);
        return GetTypedValue();
    }

    bool SetValue(TValue value)
    {
        return UpdateValue(value, false);
    }

    bool SetFromString(std::string_view text) override
    {
        TValue value{};
        return ConsoleVariableDetail::ParseValue(text, value) && SetValue(value);
    }

    bool GetBool() const override { return static_cast<bool>(GetValue()); }
    std::string GetString() const override { return FormatValue(GetValue()); }
    bool SetConfigFromString(std::string_view text) override
    {
        TValue value{};
        return ConsoleVariableDetail::ParseValue(text, value) && UpdateValue(value, true);
    }
    std::string GetConfigString() const override
    {
        std::scoped_lock lock(ValueMutex);
        return FormatValue(PendingValue.value_or(GetTypedValue()));
    }
    int GetInt() const override { return static_cast<int>(GetValue()); }
    float GetFloat() const override { return static_cast<float>(GetValue()); }
    uint32_t GetUInt() const override { return static_cast<uint32_t>(GetValue()); }

private:
    bool UpdateValue(TValue value, bool configEdit)
    {
        std::scoped_lock lock(ValueMutex);
        const bool readOnly = !CanSetValue();
        if ((readOnly && !configEdit) || !IsValidValue(value)) return false;
        if (value == PendingValue.value_or(GetTypedValue())) return true;
        if (!SaveConfigValue(FormatValue(value))) return false;
        if (readOnly) PendingValue = value;
        else SetTypedValue(value);
        return true;
    }

    static std::string FormatValue(TValue value)
    {
        if constexpr (std::is_same_v<TValue, bool>) return value ? "true" : "false";
        else
        {
            char buffer[128];
            const auto result = [&]
            {
                if constexpr (std::is_enum_v<TValue>)
                    return std::to_chars(buffer, buffer + sizeof(buffer), static_cast<std::underlying_type_t<TValue>>(value));
                else return std::to_chars(buffer, buffer + sizeof(buffer), value);
            }();
            if (result.ec != std::errc{}) throw std::runtime_error("Cannot format console variable");
            return {buffer, result.ptr};
        }
    }
protected:
    bool IsValidValue(TValue value) const
    {
        if constexpr (std::is_enum_v<TValue> && ConsoleVariableDetail::EnumSize<TValue>::Count > 0)
        {
            using Underlying = std::underlying_type_t<TValue>;
            const auto index = static_cast<Underlying>(value);
            if (index < static_cast<Underlying>(ConsoleVariableDetail::EnumFirst<TValue>::Value)) return false;
            if (index >= static_cast<Underlying>(TValue::Count)) return false;
        }
        if constexpr (std::is_floating_point_v<TValue>)
            if (!std::isfinite(value)) return false;
        return !Validate || Validate(value);
    }

    virtual TValue GetTypedValue() const = 0;
    virtual void SetTypedValue(TValue Value) = 0;

private:
    Validator Validate;
    mutable std::mutex ValueMutex;
    std::optional<TValue> PendingValue;
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
