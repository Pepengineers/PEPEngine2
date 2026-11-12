#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>

struct GDX12ParticleTime
{
    float Delta;
    std::uint32_t Substeps;
};

inline GDX12ParticleTime ResolveParticleTime(float delta, float scale, float maximumStep,
    std::uint32_t substeps, bool paused)
{
    if (!std::isfinite(delta) || delta < 0 || !std::isfinite(scale) || scale < 0 ||
        !std::isfinite(maximumStep) || maximumStep <= 0 || !substeps)
        throw std::invalid_argument("Invalid particle time input");
    const double seconds = paused ? 0 : double(delta) * scale;
    const double intervals = (std::max)(1.0, std::ceil(seconds / maximumStep));
    if (!std::isfinite(seconds) || seconds > (std::numeric_limits<float>::max)() ||
        intervals > (std::numeric_limits<std::uint32_t>::max)() / substeps)
        throw std::overflow_error("Particle simulation step count overflow");
    return {static_cast<float>(seconds), static_cast<std::uint32_t>(intervals) * substeps};
}
