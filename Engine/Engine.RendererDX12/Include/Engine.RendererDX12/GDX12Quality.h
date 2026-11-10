#pragma once
#include <cstdint>
#include "Common/EnumMetadata.h"

enum class GDX12Quality : std::uint8_t { Off, Low, Mid, High, Epic, Count };
PEP_ENUM_METADATA(GDX12Quality,
    PEP_DISPLAY_NAME(Mid, "Medium")
)

// Atmosphere and clouds retain their original four choices.
enum class GDX12LegacyQuality : std::uint8_t { Off, Low, Mid, High, Count };
PEP_ENUM_METADATA(GDX12LegacyQuality,
    PEP_DISPLAY_NAME(Mid, "Medium")
)
