#pragma once

#include "Common/ConsoleVariables.h"
#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <vector>

struct LoadingMeasurements
{
    double ImportSeconds = 0;
    double TextureSeconds = 0;
    double PublishSeconds = 0;
    size_t TextureCount = 0;
    size_t MaxImportTasks = 0;
    size_t MaxTextureTasks = 0;
};
inline LoadingMeasurements LastLoadingMeasurements;

inline AutoConsoleVariable<int> LoadingTransientBudgetMiB(
    L"Loading.TransientBudgetMiB", 1024, L"Estimated temporary CPU/upload memory budget during scene loading, in MiB",
    EConsoleVariableFlags::Default, [](int value) { return value > 0; });

inline std::uint64_t GetLoadingBudgetBytes()
{
    const int value = LoadingTransientBudgetMiB.GetInt();
    if (value <= 0) throw std::runtime_error("Loading.TransientBudgetMiB must be a positive integer");
    return static_cast<std::uint64_t>(value) * 1024u * 1024u;
}

inline bool ApplyLoadingArgument(std::wstring_view argument)
{
    constexpr std::wstring_view prefix = L"-Loading.TransientBudgetMiB=";
    if (argument.substr(0, prefix.size()) != prefix) return false;
    const auto digits = argument.substr(prefix.size());
    if (digits.empty()) throw std::runtime_error("Missing Loading.TransientBudgetMiB value");
    int value = 0;
    for (const wchar_t digit : digits)
    {
        if (digit < L'0' || digit > L'9' || value > ((std::numeric_limits<int>::max)() - (digit - L'0')) / 10)
            throw std::runtime_error("Invalid Loading.TransientBudgetMiB value");
        value = value * 10 + (digit - L'0');
    }
    if (!value) throw std::runtime_error("Loading.TransientBudgetMiB must be positive");
    if (!LoadingTransientBudgetMiB.SetValue(value)) throw std::runtime_error("Cannot set Loading.TransientBudgetMiB");
    return true;
}

inline size_t LoadingBatchEnd(const std::vector<std::uint64_t>& estimates, size_t begin, std::uint64_t budget, size_t maxTasks)
{
    std::uint64_t used = 0;
    size_t end = begin;
    while (end < estimates.size() && end - begin < maxTasks)
    {
        const auto bytes = estimates[end];
        if (bytes > budget - used) return end == begin ? begin + 1 : end;
        used += bytes;
        ++end;
    }
    return end;
}
