#include "../../../Engine/Engine.RendererDX12/Include/Engine.RendererDX12/GDX12ParticleTime.h"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

static void Check(bool value) { if (!value) throw std::runtime_error("Particle time contract failed"); }

int main()
{
    const auto normal = ResolveParticleTime(0.02f, 1, 0.1f, 4, false);
    Check(normal.Delta == 0.02f && normal.Substeps == 4);
    const auto slow = ResolveParticleTime(0.25f, 1, 0.1f, 4, false);
    Check(slow.Delta == 0.25f && slow.Substeps == 12);
    Check(slow.Delta / slow.Substeps <= 0.1f / 4);
    const auto scaled = ResolveParticleTime(0.05f, 3, 0.1f, 2, false);
    Check(std::abs(scaled.Delta - 0.15f) < 1e-6f && scaled.Substeps == 4);
    Check(ResolveParticleTime(1, 1, 0.1f, 1, true).Delta == 0);
    Check(ResolveParticleTime(1, 0, 0.1f, 4, false).Substeps == 4);
    double elapsed = 0;
    for (const float delta : {0.02f, 0.02f, 0.25f, 0.01f})
        elapsed += ResolveParticleTime(delta, 1, 0.1f, 2, false).Delta;
    Check(std::abs(elapsed - 0.3) < 1e-6);
    bool rejected = false;
    try { ResolveParticleTime(-1, 1, 0.1f, 1, false); } catch (const std::invalid_argument&) { rejected = true; }
    Check(rejected);
    for (const auto invalid : {0.0f, -1.0f, (std::numeric_limits<float>::infinity)()})
    {
        rejected = false;
        try { ResolveParticleTime(1, 1, invalid, 1, false); } catch (const std::invalid_argument&) { rejected = true; }
        Check(rejected);
    }
    rejected = false;
    try { ResolveParticleTime(1, 1, 0.1f, 0, false); } catch (const std::invalid_argument&) { rejected = true; }
    Check(rejected);
    rejected = false;
    try { ResolveParticleTime(1, 1, 0.1f, (std::numeric_limits<std::uint32_t>::max)(), false); }
    catch (const std::overflow_error&) { rejected = true; }
    Check(rejected);
    std::cout << "Particle time tests passed\n";
}
