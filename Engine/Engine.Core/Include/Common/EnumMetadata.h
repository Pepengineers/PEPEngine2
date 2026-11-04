#pragma once

#include <array>
#include <string_view>
#include <utility>

// Declare beside the enum, in the same namespace; unspecified names use the C++ identifier.
#define PEP_ENUM_METADATA(Type, ...) \
    inline constexpr std::string_view EnumDisplayName(Type value) \
    { \
        using EnumType = Type; \
        switch (value) { __VA_ARGS__ default: return {}; } \
    }
#define PEP_DISPLAY_NAME(Value, Name) case EnumType::Value: return Name;

namespace EnumMetadataDetail
{
    template <class T>
    constexpr std::string_view EnumDisplayName(T) { return {}; }

    template <class T, T Value>
    constexpr std::string_view Name()
    {
        constexpr auto displayName = EnumDisplayName(Value);
        if constexpr (!displayName.empty()) return displayName;
        else
        {
            // MSVC exposes the declared identifier even for function-local enums.
            constexpr std::string_view signature = __FUNCSIG__;
            constexpr auto end = signature.rfind(">(void)");
            constexpr auto begin = signature.rfind("::", end) + 2;
            constexpr auto name = signature.substr(begin, end - begin);
            static_assert(!name.empty() && name.find_first_not_of(
                "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") == std::string_view::npos,
                "Console enum values must be named and contiguous from First to Count");
            return name;
        }
    }

    template <class T, int First, size_t... Indices>
    constexpr auto Names(std::index_sequence<Indices...>)
    {
        return std::array<std::string_view, sizeof...(Indices)>{
            Name<T, static_cast<T>(First + static_cast<int>(Indices))>()... };
    }
}
