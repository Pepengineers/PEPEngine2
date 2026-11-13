#include "Engine.Core/Logging/Logger.h"
#include "Engine.RendererDX12/GDX12ReflectionProbes.h"
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <vector>

namespace
{
std::vector<uint8_t> Read(const std::filesystem::path &file)
{
    std::ifstream input(file, std::ios::binary);
    if (!input)
        throw std::runtime_error("Missing native probe evidence: " + file.string());
    return {std::istreambuf_iterator<char>(input), {}};
}
float Half(uint16_t value)
{
    const UINT exponent = (value >> 10) & 31, mantissa = value & 1023;
    if (exponent == 31)
        throw std::runtime_error("Nonfinite native probe radiance");
    const float magnitude =
        exponent ? std::ldexp(float(1024 + mantissa), int(exponent) - 25) : std::ldexp(float(mantissa), -24);
    return value & 0x8000 ? -magnitude : magnitude;
}
} // namespace
// Prepared for the next explicitly authorized validation session. These checks
// consume the new executor's GPU readbacks; they do not assert old-pass baseline
// equivalence, temporal/cache scenarios or a successful acceptance run.
void CheckReflectionGpuEvidence(const std::filesystem::path &root)
{
    const size_t metadataBytes = MaxReflectionProbes * sizeof(GDX12ReflectionProbeData);
    for (UINT frame = 0; frame < 8; ++frame)
    {
        const auto stem = "geometry-" + std::to_string(frame);
        const auto serial = Read(root / (stem + "-serial-probes.bin"));
        const auto parallel = Read(root / (stem + "-parallel-probes.bin"));
        if (serial != parallel || parallel.size() <= metadataBytes)
            throw std::runtime_error("Probe serial/parallel metadata or valid mip payload mismatch");
        GDX12ReflectionProbeData probe;
        std::memcpy(&probe, parallel.data(), sizeof(probe));
        if (!(probe.Intensity > 0) || !std::isfinite(probe.Intensity) || probe.MaxMip < probe.MinMip)
            throw std::runtime_error("Fixture probe was not published for lighting");
        bool lit = false;
        for (size_t offset = metadataBytes; offset + 8 <= parallel.size(); offset += 8)
        {
            uint16_t pixel[4];
            std::memcpy(pixel, parallel.data() + offset, 8);
            for (UINT channel = 0; channel < 3; ++channel)
                lit |= Half(pixel[channel]) > 0;
        }
        if (frame == 0 && !lit)
            throw std::runtime_error("Native probe capture contains no lit mesh/grass radiance");
    }
    Log::Info("MetaRenderGraph", "PASS ReflectionGpuTests readbacks: native serial/parallel metadata and valid HDR "
                                 "mips; lit geometry; original baseline/temporal acceptance remains separate");
}
