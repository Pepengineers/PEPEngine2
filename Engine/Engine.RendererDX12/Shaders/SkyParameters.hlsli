#ifndef SKY_PARAMETERS_HLSLI
#define SKY_PARAMETERS_HLSLI
// Hillaire 2020 adaptation; upstream MIT license: External/SkyAtmosphere/LICENSE.
struct SkyParameters
{
    float4 RayleighHeight;
    float4 MieScatteringHeight;
    float4 MieAbsorptionG;
    float4 OzoneCenter;
    float4 GroundWidth;
    float4 CenterRadius;
    float4 Atmosphere;
    float4 SunDirection;
    float4 SunIlluminance;
    float4 CameraPosition;
    float4 Box;
    uint4 Samples;
    uint4 Mode;
};
static const float SKY_PI = 3.14159265358979323846f;

float2 SkySphereRoots(float3 origin, float3 direction, float radius)
{
    float b = dot(origin, direction);
    float c = dot(origin, origin) - radius * radius;
    float discriminant = b * b - c;
    if (discriminant < 0) return -1;
    float d = sqrt(discriminant);
    return float2(-b - d, -b + d);
}

float2 SkyTransmittanceUV(float radius, float mu, float bottom, float top, uint2 size)
{
    float H = sqrt(max(0, top * top - bottom * bottom));
    float rho = sqrt(max(0, radius * radius - bottom * bottom));
    float d = max(0, -radius * mu + sqrt(max(0, radius * radius * (mu * mu - 1) + top * top)));
    float2 unit = saturate(float2((d - (top - radius)) / max(rho + H - (top - radius), 1e-6f), rho / H));
    return (unit * (size - 1) + 0.5f) / size;
}

void SkyTransmittanceParameters(float2 unit, float bottom, float top, out float radius, out float mu)
{
    float H = sqrt(top * top - bottom * bottom);
    float rho = H * unit.y;
    radius = sqrt(rho * rho + bottom * bottom);
    float d = top - radius + unit.x * (rho + H - (top - radius));
    mu = d < 1e-6f ? 1 : clamp((H * H - rho * rho - d * d) / (2 * radius * d), -1, 1);
}

float3 SkyPlanetPosition(float3 worldCm, SkyParameters sky)
{
    return (worldCm - sky.CenterRadius.xyz) * 0.00001f;
}

// Exact integral for a constant extinction segment, including zero coefficients.
float3 SkySegmentIntegral(float3 extinction, float distance)
{
    float3 x = extinction * distance;
    float3 regular = (1 - exp(-x)) / max(extinction, 1e-20f);
    return float3(x.x < 1e-3f ? distance * (1 - x.x * 0.5f) : regular.x,
        x.y < 1e-3f ? distance * (1 - x.y * 0.5f) : regular.y,
        x.z < 1e-3f ? distance * (1 - x.z * 0.5f) : regular.z);
}

void SkyViewBasis(float3 camera, out float3 up, out float3 east, out float3 north)
{
    up = normalize(camera);
    east = normalize(cross(abs(up.z) < 0.99f ? float3(0, 0, 1) : float3(1, 0, 0), up));
    north = cross(up, east);
}
float SkyHorizonAngle(float radius, float bottom)
{
    return acos(-sqrt(saturate(1 - bottom * bottom / max(radius * radius, bottom * bottom))));
}
float2 SkyViewUV(float3 direction, float3 camera, float bottom)
{
    float3 up, east, north;
    SkyViewBasis(camera, up, east, north);
    float angle = acos(clamp(dot(direction, up), -1, 1));
    float horizon = SkyHorizonAngle(length(camera), bottom);
    float v = angle < horizon ? 0.5f * (1 - sqrt(saturate(1 - angle / horizon))) :
        0.5f + 0.5f * sqrt(saturate((angle - horizon) / max(SKY_PI - horizon, 1e-4f)));
    return float2(atan2(dot(direction, north), dot(direction, east)) / (2 * SKY_PI) + 0.5f, v);
}
float3 SkyViewDirection(float2 uv, float3 camera, float bottom)
{
    float3 up, east, north;
    SkyViewBasis(camera, up, east, north);
    float horizon = SkyHorizonAngle(length(camera), bottom);
    float angle = uv.y < 0.5f ? horizon * (1 - pow(1 - 2 * uv.y, 2)) :
        horizon + (SKY_PI - horizon) * pow(2 * uv.y - 1, 2);
    float phi = (uv.x - 0.5f) * 2 * SKY_PI;
    return up * cos(angle) + (east * cos(phi) + north * sin(phi)) * sin(angle);
}
#endif
