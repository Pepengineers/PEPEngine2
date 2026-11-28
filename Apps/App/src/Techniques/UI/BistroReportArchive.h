#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace BistroReport
{
std::string Escape(std::string_view text);
std::filesystem::path WriteArchive(const std::filesystem::path& root, const std::filesystem::path& run,
    std::filesystem::file_time_type indexedAt, std::filesystem::path archiveStem, std::string body,
    std::vector<std::filesystem::path> measuredPasses, std::vector<std::filesystem::path> completedPasses);
std::string PassSourceMarker(size_t index);
}
