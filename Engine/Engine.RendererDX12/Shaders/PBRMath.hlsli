#ifndef PBR_MATH_HLSLI
#define PBR_MATH_HLSLI

struct PBRSurface
{
    float3 Albedo, Normal, F0;
    float Metallic, Roughness;
};

#define PBR_PI 3.14159265358979323846f

float DistributionGGX(float nDotH, float roughness)
{
    float alpha = roughness * roughness;
    float alphaSquared = alpha * alpha;
    float denominator = (1.0f - nDotH * nDotH) + nDotH * nDotH * alphaSquared;
    return alphaSquared / max(PBR_PI * denominator * denominator, 1e-12f);
}

float SmithGGXVisibility(float nDotV, float nDotL, float roughness)
{
    float alpha = roughness * roughness;
    float alphaSquared = alpha * alpha;
    float lambdaV = nDotL * sqrt(nDotV * nDotV * (1.0f - alphaSquared) + alphaSquared);
    float lambdaL = nDotV * sqrt(nDotL * nDotL * (1.0f - alphaSquared) + alphaSquared);
    return 0.5f / max(lambdaV + lambdaL, 1e-6f);
}

float FresnelSchlickFactor(float vDotH)
{
    float f = 1.0f - vDotH;
    return f * f * f * f * f;
}

float GGXSampleCosTheta(float xiY, float roughness)
{
    float alpha = roughness * roughness;
    return sqrt((1.0f - xiY) / (1.0f + (alpha * alpha - 1.0f) * xiY));
}

float SplitSumVisibility(float nDotV, float nDotL, float nDotH, float vDotH, float roughness)
{
    return 4.0f * nDotL * SmithGGXVisibility(nDotV, nDotL, roughness) * vDotH / max(nDotH, 1e-5f);
}

float ToneMapToSRGB(float value)
{
    value = max(value, 0.0f);
    float mapped = value / (1.0f + value);
    return mapped <= 0.0031308f ? 12.92f * mapped : 1.055f * pow(mapped, 1.0f / 2.4f) - 0.055f;
}

float3 ToneMapToSRGB(float3 color)
{
    return float3(ToneMapToSRGB(color.r), ToneMapToSRGB(color.g), ToneMapToSRGB(color.b));
}

#endif
