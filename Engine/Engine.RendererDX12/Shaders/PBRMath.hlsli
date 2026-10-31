#ifndef PBR_MATH_HLSLI
#define PBR_MATH_HLSLI

static const float PBR_PI = 3.14159265358979323846f;

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

float LinearToSRGB(float value)
{
    return value <= 0.0031308f ? value * 12.92f : 1.055f * pow(value, 1.0f / 2.4f) - 0.055f;
}

float ToneMapToSRGB(float value)
{
    value = max(value, 0.0f);
    return LinearToSRGB(value / (1.0f + value));
}

#endif
