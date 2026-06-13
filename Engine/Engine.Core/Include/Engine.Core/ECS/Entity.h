#pragma once

#include <cstdint>
#include <cstddef>
#include <limits>

using Entity = uint32_t;

static constexpr Entity InvalidEntity = 0;
static constexpr size_t InvalidIndex = (std::numeric_limits<size_t>::max)();