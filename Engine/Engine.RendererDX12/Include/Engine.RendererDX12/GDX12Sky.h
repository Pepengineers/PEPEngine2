#pragma once
#include <directxtk/SimpleMath.h>
#include <cstdint>
#include <filesystem>
#include <vector>

struct GDX12SkyBoxSettings
{
    std::filesystem::path Texture = "Textures/Environment/grasscube1024.dds";
    float Intensity = 1.0f;
    float RotationDegrees = 0.0f;
    uint64_t Revision = 0;
};
struct GDX12SkyAtmosphereSettings
{
    DirectX::SimpleMath::Vector3 GroundOrigin = DirectX::SimpleMath::Vector3::Zero;
    float PlanetRadiusKm = 6360.0f, AtmosphereHeightKm = 100.0f;
    float RayleighScale = 1.0f, MieScale = 1.0f, OzoneScale = 1.0f;
    float RayleighHeightKm = 8.0f, MieHeightKm = 1.2f;
    float MieAnisotropy = 0.8f, MultipleScattering = 1.0f;
    float AerialDistanceKm = 32.0f, SunAngularRadiusDegrees = 0.266f;
    DirectX::SimpleMath::Vector3 GroundAlbedo = DirectX::SimpleMath::Vector3(0.3f);
    bool AerialPerspective = true;
};
template<class T> struct GDX12SkyComponentDesc { uint64_t Entity = 0; T Settings; };
struct GDX12SkyDesc
{
    uint64_t World = 0;
    std::vector<GDX12SkyComponentDesc<GDX12SkyBoxSettings>> Boxes;
    std::vector<GDX12SkyComponentDesc<GDX12SkyAtmosphereSettings>> Atmospheres;
};
