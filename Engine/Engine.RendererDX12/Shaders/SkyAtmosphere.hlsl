// Adapted from Hillaire's EGSR 2020 reference (MIT), see External/SkyAtmosphere.
#include "CBufferStructures.hlsl"
#include "SkyParameters.hlsli"
ConstantBuffer<SkyParameters> Sky : register(b0);
ConstantBuffer<CameraCB> Camera : register(b1);
Texture2D<float4> Transmittance : register(t0);
Texture2D<float4> MultipleScattering : register(t1);
Texture2D<float4> SkyView : register(t2);
TextureCube<float4> BoxSource : register(t3);
SamplerState LinearClamp : register(s0);
RWTexture2D<float4> Output2D : register(u0);
RWTexture3D<float4> AerialRadiance : register(u1);
RWTexture3D<float4> AerialTransmittance : register(u2);
RWTexture2DArray<float4> OutputCube : register(u3);

void Medium(float3 position, out float3 rayleigh, out float3 mie, out float3 extinction)
{
    float height = max(0, length(position) - Sky.CenterRadius.w);
    rayleigh = Sky.RayleighHeight.rgb * exp(-height / Sky.RayleighHeight.w);
    float mieDensity = exp(-height / Sky.MieScatteringHeight.w);
    mie = Sky.MieScatteringHeight.rgb * mieDensity;
    float ozoneDensity = saturate(1 - abs(height - Sky.OzoneCenter.w) / Sky.GroundWidth.w);
    extinction = rayleigh + mie + Sky.MieAbsorptionG.rgb * mieDensity + Sky.OzoneCenter.rgb * ozoneDensity;
}

float3 ToSun(float3 position, float3 sun)
{
    float radius = length(position);
    float2 ground = SkySphereRoots(position, sun, Sky.CenterRadius.w);
    if (ground.x > 0.001f) return 0;
    uint w, h;
    Transmittance.GetDimensions(w, h);
    return Transmittance.SampleLevel(LinearClamp, SkyTransmittanceUV(radius, dot(position, sun) / radius,
        Sky.CenterRadius.w, Sky.Atmosphere.x, uint2(w, h)), 0).rgb;
}

bool ClipRay(inout float3 origin, float3 direction, float limit, out float distance, out bool groundHit)
{
    distance = 0;
    groundHit = false;
    float radius = length(origin);
    if (radius < Sky.CenterRadius.w + 0.001f)
        origin = normalize(origin) * (Sky.CenterRadius.w + 0.001f);
    float2 top = SkySphereRoots(origin, direction, Sky.Atmosphere.x);
    if (top.y <= 0) return false;
    float start = max(0, top.x);
    if (start >= limit) return false;
    float2 ground = SkySphereRoots(origin, direction, Sky.CenterRadius.w);
    float end = min(top.y, limit);
    if (ground.x > 0 && ground.x < end)
    {
        end = ground.x;
        groundHit = true;
    }
    origin += direction * start;
    distance = max(0, end - start);
    return distance > 0;
}

struct Integration
{
    float3 Radiance;
    float3 Transmittance;
    float3 ScatteringAsOne;
};
Integration Integrate(float3 origin, float3 direction, float3 sun, float limit, uint samples, bool multi, bool ground)
{
    Integration result = (Integration)0;
    result.Transmittance = 1;
    float distance;
    bool groundHit;
    if (!ClipRay(origin, direction, limit, distance, groundHit)) return result;
    float cosine = dot(direction, sun);
    float rayPhase = 3 * (1 + cosine * cosine) / (16 * SKY_PI);
    float g = Sky.MieAbsorptionG.w;
    float miePhase = 3 * (1 - g * g) * (1 + cosine * cosine) /
        (8 * SKY_PI * (2 + g * g) * pow(max(1 + g * g - 2 * g * cosine, 1e-4f), 1.5f));
    if (!multi) rayPhase = miePhase = 1 / (4 * SKY_PI);
    for (uint i = 0; i < samples; ++i)
    {
        float t0 = pow(float(i) / samples, 2) * distance;
        float t1 = pow(float(i + 1) / samples, 2) * distance;
        float dt = t1 - t0;
        float3 position = origin + direction * lerp(t0, t1, 0.5f);
        float3 rayleigh, mie, extinction;
        Medium(position, rayleigh, mie, extinction);
        float3 source = ToSun(position, sun) * (rayleigh * rayPhase + mie * miePhase);
        if (multi)
        {
            uint w, h;
            MultipleScattering.GetDimensions(w, h);
            float2 unit = float2(dot(normalize(position), sun) * 0.5f + 0.5f,
                saturate((length(position) - Sky.CenterRadius.w) / (Sky.Atmosphere.x - Sky.CenterRadius.w)));
            float2 uv = (unit * (uint2(w, h) - 1) + 0.5f) / uint2(w, h);
            source += MultipleScattering.SampleLevel(LinearClamp, uv, 0).rgb * (rayleigh + mie);
        }
        float3 integrated = result.Transmittance * SkySegmentIntegral(extinction, dt);
        result.Radiance += source * integrated;
        result.ScatteringAsOne += (rayleigh + mie) * integrated;
        result.Transmittance *= exp(-extinction * dt);
    }
    if (ground && groundHit)
    {
        float3 position = origin + direction * distance;
        result.Radiance += result.Transmittance * Sky.GroundWidth.rgb * ToSun(position, sun) *
            saturate(dot(normalize(position), sun)) / SKY_PI;
    }
    return result;
}

[numthreads(8, 8, 1)]
void TransmittanceCS(uint3 id : SV_DispatchThreadID)
{
    uint w, h;
    Output2D.GetDimensions(w, h);
    if (any(id.xy >= uint2(w, h))) return;
    float radius, mu;
    SkyTransmittanceParameters(float2(id.xy) / (uint2(w, h) - 1), Sky.CenterRadius.w, Sky.Atmosphere.x, radius, mu);
    float3 origin = float3(0, radius, 0);
    float3 direction = float3(sqrt(saturate(1 - mu * mu)), mu, 0);
    float distance = max(0, SkySphereRoots(origin, direction, Sky.Atmosphere.x).y);
    float3 opticalDepth = 0;
    for (uint i = 0; i < Sky.Samples.x; ++i)
    {
        float3 rayleigh, mie, extinction;
        Medium(origin + direction * ((i + 0.5f) * distance / Sky.Samples.x), rayleigh, mie, extinction);
        opticalDepth += extinction * (distance / Sky.Samples.x);
    }
    Output2D[id.xy] = float4(exp(-opticalDepth), 1);
}

[numthreads(8, 8, 1)]
void MultipleScatteringCS(uint3 id : SV_DispatchThreadID)
{
    uint w, h;
    Output2D.GetDimensions(w, h);
    if (any(id.xy >= uint2(w, h))) return;
    float2 unit = float2(id.xy) / (uint2(w, h) - 1);
    float mu = unit.x * 2 - 1;
    float3 sun = float3(sqrt(saturate(1 - mu * mu)), mu, 0);
    float3 origin = float3(0, lerp(Sky.CenterRadius.w + 0.001f, Sky.Atmosphere.x - 0.001f, unit.y), 0);
    float3 luminance = 0, scattering = 0;
    for (uint i = 0; i < Sky.Samples.z; ++i)
    {
        float y = 1 - 2 * (i + 0.5f) / Sky.Samples.z;
        float phi = 2 * SKY_PI * frac(i * 0.61803398875f);
        float3 direction = float3(cos(phi) * sqrt(1 - y * y), y, sin(phi) * sqrt(1 - y * y));
        Integration ray = Integrate(origin, direction, sun, 1e7f, Sky.Samples.w, false, true);
        luminance += ray.Radiance;
        scattering += ray.ScatteringAsOne;
    }
    luminance /= Sky.Samples.z;
    scattering /= Sky.Samples.z;
    // Hillaire's isotropic geometric series (equation 10), with a finite energy bound.
    Output2D[id.xy] = float4(luminance / max(1 - scattering, 0.001f), 1);
}

[numthreads(8, 8, 1)]
void SkyViewCS(uint3 id : SV_DispatchThreadID)
{
    uint w, h;
    Output2D.GetDimensions(w, h);
    if (any(id.xy >= uint2(w, h))) return;
    float2 uv = (float2(id.xy) + 0.5f) / uint2(w, h);
    float3 direction = SkyViewDirection(uv, Sky.CameraPosition.xyz, Sky.CenterRadius.w);
    Integration result = Integrate(Sky.CameraPosition.xyz, direction, Sky.SunDirection.xyz, 1e7f, Sky.Samples.y, true, true);
    Output2D[id.xy] = float4(result.Radiance * Sky.SunIlluminance.rgb, 1);
}

[numthreads(4, 4, 4)]
void AerialCS(uint3 id : SV_DispatchThreadID)
{
    uint w, h, d;
    AerialRadiance.GetDimensions(w, h, d);
    if (any(id >= uint3(w, h, d))) return;
    float2 uv = (float2(id.xy) + 0.5f) / uint2(w, h);
    float4 world = mul(float4(uv * float2(2, -2) + float2(-1, 1), 0.000001f, 1), Camera.InvViewProj);
    float3 direction = normalize(world.xyz / world.w - Camera.CameraLocation);
    float distance = pow((id.z + 0.5f) / d, 2) * Sky.Atmosphere.y;
    Integration result = Integrate(Sky.CameraPosition.xyz, direction, Sky.SunDirection.xyz, distance, Sky.Mode.y, true, false);
    AerialRadiance[id] = float4(result.Radiance * Sky.SunIlluminance.rgb, 1);
    AerialTransmittance[id] = float4(result.Transmittance, 1);
}

float3 CubeDirection(uint3 id, uint size)
{
    float2 uv = (float2(id.xy) + 0.5f) / size * 2 - 1;
    if (id.z == 0) return normalize(float3(1, -uv.y, -uv.x));
    if (id.z == 1) return normalize(float3(-1, -uv.y, uv.x));
    if (id.z == 2) return normalize(float3(uv.x, 1, uv.y));
    if (id.z == 3) return normalize(float3(uv.x, -1, -uv.y));
    if (id.z == 4) return normalize(float3(uv.x, -uv.y, 1));
    return normalize(float3(-uv.x, -uv.y, -1));
}
[numthreads(8, 8, 1)]
void SourceCubeCS(uint3 id : SV_DispatchThreadID)
{
    uint w, h, faces;
    OutputCube.GetDimensions(w, h, faces);
    if (any(id.xy >= uint2(w, h))) return;
    float3 direction = CubeDirection(id, w);
    float3 color = 0;
    if (Sky.Mode.x == 1)
    {
        direction.xz = float2(direction.x * Sky.Box.z - direction.z * Sky.Box.y,
            direction.x * Sky.Box.y + direction.z * Sky.Box.z);
        uint sourceWidth, sourceHeight, sourceLevels;
        BoxSource.GetDimensions(0, sourceWidth, sourceHeight, sourceLevels);
        float lod = max(0, log2(float(sourceWidth) / w));
        color = BoxSource.SampleLevel(LinearClamp, direction, lod).rgb * Sky.Box.x;
    }
    // shortcut: atmosphere source mips sample sky-view directly; add solid-angle downsampling if the horizon aliases.
    else if (Sky.Mode.x == 2)
        color = SkyView.SampleLevel(LinearClamp, SkyViewUV(direction, Sky.CameraPosition.xyz, Sky.CenterRadius.w), 0).rgb;
    // Solar disk is analytic direct lighting, excluded from diffuse/specular IBL to avoid duplicate sun energy.
    OutputCube[id] = float4(color, 1);
}
