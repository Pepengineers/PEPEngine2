#pragma once
#include "Components/BistroGrassComponents.h"
#include "BistroArch.h"

#include <cstdint>
#include "App.Base/ECS/World.h"
#include "App.Base/Modules/RenderModule.h"
#include "Engine.Core/BenchmarkEngine.h"
#include "Common/ConsoleVariables.h"
#include <cmath>
#include <tuple>
#include <vector>

namespace BistroGrass
{
    inline AutoConsoleVariable<int> Count(L"App.Grass.Count", 1,
        L"Bistro arch grass fields, 0 disables, maximum 16",
        EConsoleVariableFlags::Default, [](int value) { return value >= 0 && value <= 16; });

    enum class Geometry : std::uint8_t { Ribbon, CrossedRibbons, Count };
    enum class WindAlgorithm : std::uint8_t { Analytic, ProjectedGrid, Count };
    inline AutoConsoleVariable<Geometry> Implementation(L"App.Grass.Implementation", Geometry::Ribbon,
        L"0 Ribbon, 1 CrossedRibbons; runtime grass geometry");
    inline AutoConsoleVariable<float> ExtentsXCm(L"App.Grass.ExtentsXCm", BistroArch::HalfWidthCm,
        L"Patch half-width along local X in cm, 1..100000", EConsoleVariableFlags::Default,
        [](float value) { return value >= 1 && value <= 100000; });
    inline AutoConsoleVariable<float> ExtentsZCm(L"App.Grass.ExtentsZCm", BistroArch::HalfLengthCm,
        L"Patch half-width along local Z in cm, 1..100000", EConsoleVariableFlags::Default,
        [](float value) { return value >= 1 && value <= 100000; });
    inline AutoConsoleVariable<float> Density(L"App.Grass.DensityPerSquareMetre", 1500,
        L"Blades per square metre, 0..100000; actual count = min(ceil(area*density), MaxBlades)",
        EConsoleVariableFlags::Default, [](float value) { return value >= 0 && value <= 100000; });
    inline AutoConsoleVariable<UINT> MaxBlades(L"App.Grass.MaxBlades", 60000,
        L"Blade budget per patch, 0 inherits Render.Grass.Quality, maximum 262144",
        EConsoleVariableFlags::Default, [](UINT value) { return value <= 262144; });
    inline AutoConsoleVariable<UINT> Segments(L"App.Grass.Segments", 4,
        L"Segments per blade, 0 inherits Render.Grass.Quality, maximum 8",
        EConsoleVariableFlags::Default, [](UINT value) { return value <= 8; });
    inline AutoConsoleVariable<UINT> Seed(L"App.Grass.Seed", 7919, L"Root distribution seed; patches use distinct multiples");
    inline AutoConsoleVariable<float> HeightCm(L"App.Grass.HeightCm", 40,
        L"Blade height in cm, 0.1..10000", EConsoleVariableFlags::Default,
        [](float value) { return value >= 0.1f && value <= 10000; });
    inline AutoConsoleVariable<float> HeightVariation(L"App.Grass.HeightVariation", 0.25f,
        L"Relative random height variation, 0..1", EConsoleVariableFlags::Default,
        [](float value) { return value >= 0 && value <= 1; });
    inline AutoConsoleVariable<float> WidthCm(L"App.Grass.WidthCm", 3.0f,
        L"Blade width in cm, 0.01..1000", EConsoleVariableFlags::Default,
        [](float value) { return value >= 0.01f && value <= 1000; });
    inline AutoConsoleVariable<float> BendSeconds(L"App.Grass.BendSeconds", 0.15f,
        L"Grass wind response time in seconds, 0..10", EConsoleVariableFlags::Default,
        [](float value) { return value >= 0 && value <= 10; });
    inline AutoConsoleVariable<float> MaxBendCm(L"App.Grass.MaxBendCm", 20,
        L"Maximum blade bend in cm, 0..10000; also expands culling/wind bounds",
        EConsoleVariableFlags::Default, [](float value) { return value >= 0 && value <= 10000; });
    inline AutoConsoleVariable<float> LodDistanceCm(L"App.Grass.LodDistanceCm", 1500,
        L"Near LOD distance in cm, 1..1000000; far-card and cull distances are 2x and 4x",
        EConsoleVariableFlags::Default, [](float value) { return value >= 1 && value <= 1000000; });
    inline AutoConsoleVariable<bool> CastShadows(L"App.Grass.CastShadows", true, L"Grass casts directional and local shadows");

    inline AutoConsoleVariable<bool> WindEnabled(L"App.Grass.Wind.Enabled", true, L"Enable each grass patch's wind field");
    inline AutoConsoleVariable<WindAlgorithm> Algorithm(L"App.Grass.Wind.Algorithm", WindAlgorithm::Analytic,
        L"0 Analytic directional gusts, 1 ProjectedGrid pressure-solved wind");
    inline AutoConsoleVariable<float> WindSpeed(L"App.Grass.Wind.SpeedCmPerSecond", 100,
        L"Base wind speed in cm/s, 0..500", EConsoleVariableFlags::Default,
        [](float value) { return value >= 0 && value <= 500; });
    inline AutoConsoleVariable<float> WindAngleDegrees(L"App.Grass.Wind.AngleDegrees", 67.5f,
        L"Wind direction in local XZ degrees, -180..180; zero points along +X",
        EConsoleVariableFlags::Default, [](float value) { return value >= -180 && value <= 180; });
    inline AutoConsoleVariable<float> GustAmplitude(L"App.Grass.Wind.GustAmplitude", 0.6f,
        L"Relative gust amplitude, 0..1", EConsoleVariableFlags::Default,
        [](float value) { return value >= 0 && value <= 1; });
    inline AutoConsoleVariable<float> GustFrequencyHz(L"App.Grass.Wind.GustFrequencyHz", 0.4f,
        L"Gust frequency in Hz, 0..100", EConsoleVariableFlags::Default,
        [](float value) { return value >= 0 && value <= 100; });
    inline AutoConsoleVariable<float> GustWavelengthCm(L"App.Grass.Wind.GustWavelengthCm", 500,
        L"Gust wavelength in cm, 1..1000000", EConsoleVariableFlags::Default,
        [](float value) { return value >= 1 && value <= 1000000; });
    inline AutoConsoleVariable<float> ForcePerSecond(L"App.Grass.Wind.ForcePerSecond", 2,
        L"Projected-grid flow forcing per second, 0..100", EConsoleVariableFlags::Default,
        [](float value) { return value >= 0 && value <= 100; });
    inline AutoConsoleVariable<float> DampingPerSecond(L"App.Grass.Wind.DampingPerSecond", 0.2f,
        L"Projected-grid velocity damping per second, 0..100", EConsoleVariableFlags::Default,
        [](float value) { return value >= 0 && value <= 100; });
    inline AutoConsoleVariable<float> TimeScale(L"App.Grass.Wind.TimeScale", 1,
        L"Wind simulation time scale, 0..10; zero freezes wind", EConsoleVariableFlags::Default,
        [](float value) { return value >= 0 && value <= 10; });
    inline AutoConsoleVariable<float> MaxStepSeconds(L"App.Grass.Wind.MaxStepSeconds", 1.0f / 60,
        L"Maximum projected-grid step in seconds, 0.0001..0.1", EConsoleVariableFlags::Default,
        [](float value) { return value >= 0.0001f && value <= 0.1f; });
    inline AutoConsoleVariable<UINT> MaxSubsteps(L"App.Grass.Wind.MaxSubsteps", 4,
        L"Maximum wind substeps per frame, 1..16", EConsoleVariableFlags::Default,
        [](UINT value) { return value >= 1 && value <= 16; });
    inline AutoConsoleVariable<UINT> GridResolution(L"App.Grass.Wind.GridResolution", 0,
        L"Projected-grid resolution, 0 inherits Render.Wind.Quality, or 8..256",
        EConsoleVariableFlags::Default, [](UINT value) { return value == 0 || (value >= 8 && value <= 256); });
    inline AutoConsoleVariable<UINT> PressureIterations(L"App.Grass.Wind.PressureIterations", 0,
        L"Projected-grid pressure solve iterations, 0 inherits Render.Wind.Quality, maximum 128",
        EConsoleVariableFlags::Default, [](UINT value) { return value <= 128; });

    struct Controls
    {
        GDX12GrassSettings Grass;
        GDX12WindSettings Wind;
        bool WindEnabled = true;

        auto GrassKey() const
        {
            return std::tie(Grass.Implementation, Grass.ExtentsCm, Grass.DensityPerSquareMetre, Grass.MaxBlades,
                Grass.Segments, Grass.Seed, Grass.HeightCm, Grass.HeightVariation, Grass.WidthCm, Grass.BendSeconds,
                Grass.MaxBendCm, Grass.LodDistanceCm, Grass.CastShadows);
        }
        auto WindKey() const
        {
            return std::tie(WindEnabled, Wind.Implementation, Wind.ExtentsCm, Wind.Direction, Wind.SpeedCmPerSecond,
                Wind.GustAmplitude, Wind.GustFrequencyHz, Wind.GustWavelengthCm, Wind.ForcePerSecond,
                Wind.DampingPerSecond, Wind.TimeScale, Wind.MaxStepSeconds, Wind.MaxSubsteps,
                Wind.GridResolution, Wind.PressureIterations);
        }
    };

    inline Controls ReadControls()
    {
        Controls controls;
        auto& grass = controls.Grass;
        grass.Implementation = static_cast<GDX12GrassImplementation>(Implementation.GetValue());
        grass.ExtentsCm = Vector2(ExtentsXCm.GetValue(), ExtentsZCm.GetValue());
        grass.DensityPerSquareMetre = Density.GetValue();
        grass.MaxBlades = MaxBlades.GetValue();
        grass.Segments = Segments.GetValue();
        grass.Seed = Seed.GetValue();
        grass.HeightCm = HeightCm.GetValue();
        grass.HeightVariation = HeightVariation.GetValue();
        grass.WidthCm = WidthCm.GetValue();
        grass.BendSeconds = BendSeconds.GetValue();
        grass.MaxBendCm = MaxBendCm.GetValue();
        grass.LodDistanceCm = LodDistanceCm.GetValue();
        grass.FarCardDistanceCm = grass.LodDistanceCm * 2;
        grass.MaxDistanceCm = grass.LodDistanceCm * 4;
        grass.CastShadows = CastShadows.GetValue();
        grass.CaptureReflections = false;
        auto& wind = controls.Wind;
        controls.WindEnabled = WindEnabled.GetValue();
        wind.Implementation = static_cast<GDX12WindImplementation>(Algorithm.GetValue());
        BoundingBox worldBounds;
        GrassLocalBounds(grass).Transform(worldBounds, Matrix::CreateRotationY(XMConvertToRadians(BistroArch::YawDegrees)));
        wind.ExtentsCm = Vector2((std::max)(90.0f, worldBounds.Extents.x), (std::max)(70.0f, worldBounds.Extents.z));
        const float angle = XMConvertToRadians(WindAngleDegrees.GetValue());
        wind.Direction = Vector2(std::cos(angle), std::sin(angle));
        wind.SpeedCmPerSecond = WindSpeed.GetValue();
        wind.GustAmplitude = GustAmplitude.GetValue();
        wind.GustFrequencyHz = GustFrequencyHz.GetValue();
        wind.GustWavelengthCm = GustWavelengthCm.GetValue();
        wind.ForcePerSecond = ForcePerSecond.GetValue();
        wind.DampingPerSecond = DampingPerSecond.GetValue();
        wind.TimeScale = TimeScale.GetValue();
        wind.MaxStepSeconds = MaxStepSeconds.GetValue();
        wind.MaxSubsteps = MaxSubsteps.GetValue();
        wind.GridResolution = GridResolution.GetValue();
        wind.PressureIterations = PressureIterations.GetValue();
        return controls;
    }

    class Scene
    {
    public:
        void Update(World& world, float)
        {
            RegisterBistroGrassComponents(world);
            auto& ecs = world.GetECS();
            const size_t requested = static_cast<size_t>(Count.GetValue());
            const auto controls = ReadControls();
            const bool grassChanged = !_haveControls || controls.GrassKey() != _controls.GrassKey();
            const bool windChanged = !_haveControls || controls.WindKey() != _controls.WindKey();
            if (!grassChanged && !windChanged && _patches.size() == requested) return;
            const size_t previousCount = _patches.size();
            while (_patches.size() > requested)
            {
                ecs.DestroyEntity(_patches.back().Grass);
                ecs.DestroyEntity(_patches.back().Wind);
                _patches.pop_back();
            }
            if (_patches.size() < requested)
            {
                auto& render = *BenchmarkEngine::GetLocator().GetModule<RenderModule>();
                if (!_material) _material = CreateMaterial(render);
                render.ReserveTransformCapacity(static_cast<UINT>(2 * (requested - _patches.size())));
            }
            while (_patches.size() < requested)
            {
                const size_t index = _patches.size();
                const auto rotation = Quaternion::CreateFromAxisAngle(Vector3::Up, XMConvertToRadians(BistroArch::YawDegrees));
                const Vector3 position = BistroArch::GrassPosition + Vector3::Transform(Vector3(
                    2 * (controls.Grass.ExtentsCm.x + controls.Grass.MaxBendCm + controls.Grass.WidthCm) * static_cast<float>(index % 4),
                    0, 2 * (controls.Grass.ExtentsCm.y + controls.Grass.MaxBendCm + controls.Grass.WidthCm) * static_cast<float>(index / 4)), rotation);
                auto wind = ecs.CreateEntity();
                wind.AddComponent<NameComponent>("BistroGrassWind" + std::to_string(index));
                wind.AddComponent<TransformComponent>(position);
                wind.AddComponent<WindFieldComponent>();

                auto grass = ecs.CreateEntity();
                grass.AddComponent<NameComponent>("BistroGrass" + std::to_string(index));
                grass.AddComponent<TransformComponent>(position);
                grass.GetComponent<TransformComponent>().Rotation.y = BistroArch::YawDegrees;
                auto& component = grass.AddComponent<GrassRenderComponent>();
                component.Material = _material;
                component.WindEntity = wind.GetId();
                _patches.push_back({grass.GetId(), wind.GetId()});
            }
            for (size_t index = 0; index < _patches.size(); ++index)
            {
                auto& grass = ecs.Get<GrassRenderComponent>(_patches[index].Grass);
                auto& wind = ecs.Get<WindFieldComponent>(_patches[index].Wind);
                if (grassChanged || index >= previousCount)
                {
                    auto settings = controls.Grass;
                    settings.Seed *= static_cast<UINT>(index + 1);
                    grass.SetSettings(settings);
                }
                else if (windChanged) grass.ResetHistory();
                if (windChanged || index >= previousCount)
                {
                    auto settings = controls.Wind;
                    settings.ResetRevision = wind.ResetRevision;
                    wind.SetSettings(settings);
                    wind.Enabled = controls.WindEnabled;
                }
            }
            _controls = controls;
            _haveControls = true;
        }

    private:
        static GDX12Material* CreateMaterial(RenderModule& render)
        {
            if (auto* material = render.GetMaterialByName("BistroGrass")) return material;
            auto* diffuse = render.GetTextureByName("BistroGrassGreen");
            if (!diffuse)
            {
                Engine::Core::TextureDesc desc;
                desc.Dimension = Engine::Core::ETextureDimension::Texture2D;
                desc.Type = Engine::Core::ETextureType::Color;
                desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                desc.Width = desc.Height = desc.ArraySize = desc.MipLevels = 1;
                Engine::Core::SubTexture pixel;
                pixel.Width = pixel.Height = 1;
                pixel.RowPitch = pixel.SlicePitch = 4;
                pixel.Data = {std::byte{70}, std::byte{145}, std::byte{35}, std::byte{255}};
                const Engine::Core::Texture texture(desc, {std::move(pixel)});
                diffuse = render.CreateTexture("BistroGrassGreen", &texture, Engine::Core::ETextureType::Color);
                render.WaitForUploads();
            }
            if (!diffuse || !diffuse->PrimaryDeviceTexture)
                throw std::runtime_error("Cannot create Bistro grass diffuse texture");
            auto* material = render.CreateMaterial("BistroGrass");
            if (!material) throw std::runtime_error("Cannot create Bistro grass material");
            material->Diffuse = diffuse;
            material->Roughness = 0.85f;
            return material;
        }

        struct Patch { Entity Grass; Entity Wind; };
        std::vector<Patch> _patches;
        GDX12Material* _material = nullptr;
        Controls _controls;
        bool _haveControls = false;
    };
}
