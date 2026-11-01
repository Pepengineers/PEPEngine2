#ifndef PEPE_CLOUD_NOISE
#define PEPE_CLOUD_NOISE
// Original implementation for PEPEngine2; no donor/Shadertoy code is incorporated.
// Standard simplex skew geometry with an integer, 256-lattice-period gradient hash.
float2 CloudGradient(int2 cell)
{
    uint2 wrapped = uint2(cell) & 255u;
    uint hash = wrapped.x * 747796405u + wrapped.y * 2891336453u + 277803737u;
    hash = (hash ^ (hash >> 16)) * 2246822519u;
    uint index = (hash ^ (hash >> 13)) & 7u;
    const float2 gradients[8] = {
        float2(1,0), float2(-1,0), float2(0,1), float2(0,-1),
        float2(0.70710678,0.70710678), float2(-0.70710678,0.70710678),
        float2(0.70710678,-0.70710678), float2(-0.70710678,-0.70710678) };
    return gradients[index];
}

float CloudSimplex(float2 position, float2 phase)
{
    const float skew = 0.366025403784f;
    const float unskew = 0.211324865405f;
    float2 lattice = position + (position.x + position.y) * skew + phase;
    int2 cell = int2(floor(lattice));
    float2 point = lattice - float2(cell);
    point -= (point.x + point.y) * unskew;
    int2 middle = point.x > point.y ? int2(1, 0) : int2(0, 1);
    float sum = 0;
    for (uint vertex = 0; vertex < 3; ++vertex)
    {
        int2 offset = vertex == 0 ? int2(0,0) : (vertex == 1 ? middle : int2(1,1));
        float2 delta = point - float2(offset) + (offset.x + offset.y) * unskew;
        float weight = max(0.5f - dot(delta, delta), 0.0f);
        sum += weight * weight * weight * weight * dot(CloudGradient(cell + offset), delta);
    }
    return clamp(70.0f * sum, -1.0f, 1.0f);
}

float CloudFractal(float2 position, float2 phase, uint octaves, uint implementation)
{
    float total = 0;
    float normalization = 0;
    float amplitude = 1;
    for (uint octave = 0; octave < octaves; ++octave)
    {
        float value = CloudSimplex(position, phase);
        value = implementation == 1 ? 1.0f - abs(value) : 0.5f + 0.5f * value;
        total += amplitude * value;
        normalization += amplitude;
        amplitude *= 0.5f;
        position = float2(position.x + position.y, position.y - position.x) * 1.41421356f;
    }
    return total / max(normalization, 1e-6f);
}

// Material shaders can call the same 2D noise directly, with their instance's uniforms.
float CloudDirectionDensity(float3 direction, float scale, float2 phase, uint octaves, uint implementation)
{
    // Direction-space projection avoids longitude seams and singularities at either pole.
    float3 weights = direction * direction;
    float3 position = direction * scale;
    return dot(weights, float3(
        CloudFractal(position.yz, phase, octaves, implementation),
        CloudFractal(position.zx, phase, octaves, implementation),
        CloudFractal(position.xy, phase, octaves, implementation)));
}
#endif
