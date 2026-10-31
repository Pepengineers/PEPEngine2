#include <algorithm>
#include <cassert>
#include <cmath>
#include <iostream>

using std::max;
using std::pow;
using std::sqrt;
#include "../../../Engine/Engine.RendererDX12/Shaders/PBRMath.hlsli"

int main()
{
    const auto near = [](float actual, float expected, float tolerance = 1e-5f)
    {
        assert(std::abs(actual - expected) <= tolerance);
    };

    near(DistributionGGX(0.3f, 1.0f), 1.0f / PBR_PI);
    near(DistributionGGX(1.0f, 0.5f), 16.0f / PBR_PI);
    near(SmithGGXVisibility(1.0f, 1.0f, 0.5f), 0.25f);
    near(FresnelSchlickFactor(0.0f), 1.0f);
    near(FresnelSchlickFactor(1.0f), 0.0f);
    near(FresnelSchlickFactor(0.5f), 0.03125f);
    near(LinearToSRGB(0.0f), 0.0f);
    near(LinearToSRGB(1.0f), 1.0f);
    near(LinearToSRGB(0.0031308f), 0.0404499f);
    near(ToneMapToSRGB(0.0f), 0.0f);
    near(ToneMapToSRGB(-1.0f), 0.0f);
    near(ToneMapToSRGB(1.0f), LinearToSRGB(0.5f));
    assert(ToneMapToSRGB(1.0f) < ToneMapToSRGB(10.0f));
    assert(ToneMapToSRGB(10.0f) < 1.0f);

    for (float roughness : { 0.045f, 0.2f, 0.5f, 1.0f })
    {
        for (float angle : { 0.0f, 0.00001f, 0.5f, 1.0f })
        {
            const float d = DistributionGGX(angle, roughness);
            const float visibility = SmithGGXVisibility(angle, 0.8f, roughness);
            assert(std::isfinite(d) && d >= 0.0f);
            assert(std::isfinite(visibility) && visibility >= 0.0f);
            near(visibility, SmithGGXVisibility(0.8f, angle, roughness));
        }
    }

    // Integrate the actual shader NDF and BRDF over the hemisphere.
    constexpr int samples = 20000;
    for (float roughness : { 0.3f, 0.5f, 1.0f })
    {
        double distributionIntegral = 0.0;
        double dielectricEnergy = 0.0;
        double metalEnergy = 0.0;
        for (int i = 0; i < samples; ++i)
        {
            const float nDotL = (i + 0.5f) / samples;
            distributionIntegral += DistributionGGX(nDotL, roughness) * nDotL * 2.0f * PBR_PI / samples;
            const float nDotH = sqrt((1.0f + nDotL) * 0.5f);
            const float f = FresnelSchlickFactor(nDotH);
            const float specular = DistributionGGX(nDotH, roughness) * SmithGGXVisibility(1.0f, nDotL, roughness);
            const float dielectricFresnel = 0.04f + 0.96f * f;
            dielectricEnergy += ((1.0f - dielectricFresnel) / PBR_PI + specular * dielectricFresnel) * nDotL * 2.0f * PBR_PI / samples;
            metalEnergy += specular * (0.9f + 0.1f * f) * nDotL * 2.0f * PBR_PI / samples;
        }
        assert(std::abs(distributionIntegral - 1.0) < 0.001);
        assert(dielectricEnergy > 0.0 && dielectricEnergy <= 1.001);
        assert(metalEnergy > 0.0 && metalEnergy <= 1.001);
    }
    std::cout << "PBR shader math: normalization, reciprocity, energy, grazing angles, and sRGB passed.\n";
}
