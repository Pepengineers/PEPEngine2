#pragma once

#include <array>
#include <cstddef>
#include <utility>

namespace BistroRender {
enum class CloudPictureQuality { Low, Medium, High, Epic, Custom, Count };

inline constexpr std::array<std::pair<int, int>, std::size_t(CloudPictureQuality::Custom)> CloudPictureSizes{{
    {512, 254}, {1024, 512}, {2048, 1024}, {4096, 2048}
}};
}
