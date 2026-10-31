#pragma once

#include <cstddef>
#include <cstdint>
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

std::uint64_t GetLoadingBudgetBytes();
bool ApplyLoadingArgument(std::wstring_view argument);

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
