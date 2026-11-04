#pragma once
#include "Engine.RendererDX12/D3DHelpers.h"
#include <filesystem>
#include <cstdint>

// Source selection belongs to the world; components never compete by iteration order.
enum class SkySource : std::uint8_t { Off, SkyBox, SkyAtmosphere };

struct SkyIBLSettings
{
    UINT CubeResolution = 0;
    UINT FilterSamples = 0;
    float Intensity = 1.0f;
};

struct SkyBoxSettings
{
    bool Enabled = true;
    std::filesystem::path Cubemap = "Textures/Environment/grasscube1024.dds";
    float RadianceMultiplier = 1.0f;
    float RotationRadians = 0.0f;
    SkyIBLSettings IBL;
};

struct SkyAtmosphereSettings
{
    bool Enabled = true;
    Vector3 PlanetCenterCm = { 0.0f, -636000000.0f, 0.0f };
    float PlanetRadiusKm = 6360.0f;
    float AtmosphereHeightKm = 100.0f;
    Vector3 RayleighScatteringPerKm = { 0.005802f, 0.013558f, 0.033100f };
    float RayleighScaleHeightKm = 8.0f;
    Vector3 MieScatteringPerKm = Vector3(0.003996f);
    Vector3 MieAbsorptionPerKm = Vector3(0.000444f);
    float MieScaleHeightKm = 1.2f;
    float MieAnisotropy = 0.8f;
    Vector3 OzoneAbsorptionPerKm = { 0.000650f, 0.001881f, 0.000085f };
    float OzoneCenterKm = 25.0f;
    float OzoneHalfWidthKm = 15.0f;
    Vector3 GroundAlbedo = Vector3(0.3f);
    Vector3 DirectionToSun = { -0.35f, 1.0f, -0.25f };
    Vector3 SunIlluminanceLux = { 120000.0f, 114000.0f, 105000.0f };
    float SunAngularRadiusRadians = 0.004675f;
    float LuminanceScale = 0.00001f;
    float AerialDistanceKm = 96.0f;
    Vector3 IBLCapturePositionCm = { 0.0f, 200.0f, 0.0f };
    bool SunCastsShadows = true;
    SkyIBLSettings IBL;
    // Zero inherits the quality preset; explicit values survive quality changes.
    UINT TransmittanceWidth = 0, TransmittanceHeight = 0;
    UINT MultipleScatteringSize = 0;
    UINT SkyViewWidth = 0, SkyViewHeight = 0;
    UINT AerialWidth = 0, AerialHeight = 0, AerialDepth = 0;
    UINT TransmittanceSamples = 0, SkySamples = 0;
    UINT MultipleScatteringDirections = 0, MultipleScatteringSamples = 0;
    UINT AerialSamples = 0;
};

void ValidateSkySettings(const SkyBoxSettings& settings);
void ValidateSkySettings(const SkyAtmosphereSettings& settings);

struct GDX12SkyState
{
    SkySource Source = SkySource::Off;
    SkyBoxSettings Box;
    SkyAtmosphereSettings Atmosphere;
    UINT Quality = 0;
    uint64_t Revision = 0;
};

// Exactly two runtime Default controls for each component technique.
UINT GetSkyBoxQuality();
UINT GetSkyAtmosphereQuality();
SkyAtmosphereSettings ResolveSkyAtmosphereSettings(const SkyAtmosphereSettings& settings, UINT quality);
SkyIBLSettings ResolveSkyIBLSettings(const SkyIBLSettings& settings, UINT quality);

// Matches SkyParameters.hlsli. All lengths below are km except CenterRadius.xyz (cm).
struct GDX12SkyConstants
{
    XMFLOAT4 RayleighHeight = {};
    XMFLOAT4 MieScatteringHeight = {};
    XMFLOAT4 MieAbsorptionG = {};
    XMFLOAT4 OzoneCenter = {};
    XMFLOAT4 GroundWidth = {};
    XMFLOAT4 CenterRadius = {};
    XMFLOAT4 Atmosphere = {}; // top radius, aerial distance, sun angular radius, IBL multiplier
    XMFLOAT4 SunDirection = {};
    XMFLOAT4 SunIlluminance = {}; // scene-linear illuminance = lux * LuminanceScale
    XMFLOAT4 CameraPosition = {};
    XMFLOAT4 Box = {}; // box: multiplier, sin(yaw), cos(yaw), 0; atmosphere: 0, 0, render width, height
    XMUINT4 Samples = {}; // transmittance, sky, MS directions, MS ray samples
    XMUINT4 Mode = {}; // source, aerial samples, IBL samples, unused
};
static_assert(sizeof(GDX12SkyConstants) == 208);
