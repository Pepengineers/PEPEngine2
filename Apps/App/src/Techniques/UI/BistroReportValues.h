#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace BistroReport
{
using Settings = std::map<std::string, std::string>;
std::optional<double> Number(const std::string& text);
const std::string& Value(const Settings& values, const std::string& key);
std::string DisplayValue(const std::string& key, const std::string& raw);
std::string SettingLabel(std::string_view key);
std::string ImageSummary(const Settings& settings);
}
