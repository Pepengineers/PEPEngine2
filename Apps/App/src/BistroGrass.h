#pragma once

#include "App.Base/ECS/World.h"
#include "App.Base/Modules/RenderModule.h"
#include "Engine.Core/BenchmarkEngine.h"
#include "Common/ConsoleVariables.h"
#include <vector>

namespace BistroGrass
{
    inline AutoConsoleVariable<int> Count(L"App.Grass.Count", 1,
        L"Bistro entrance grass patches, 0 disables, maximum 16",
        EConsoleVariableFlags::Default, [](int value) { return value >= 0 && value <= 16; });

    class Scene
    {
    public:
        void Update(World& world, float)
        {
            auto& ecs = world.GetECS();
            const size_t requested = static_cast<size_t>(Count.GetValue());
            while (_patches.size() > requested)
            {
                ecs.DestroyEntity(_patches.back().Grass);
                ecs.DestroyEntity(_patches.back().Wind);
                _patches.pop_back();
            }
            if (_patches.size() == requested) return;

            auto& render = *BenchmarkEngine::GetLocator().GetModule<RenderModule>();
            if (!_material) _material = CreateMaterial(render);
            render.ReserveTransformCapacity(static_cast<UINT>(2 * (requested - _patches.size())));
            while (_patches.size() < requested)
            {
                const size_t index = _patches.size();
                // Exterior sidewalk is at 23.162 cm here; keep the roots above its depth surface.
                const Vector3 position(-420.0f - 160.0f * static_cast<float>(index % 4), 23.3f,
                    150.0f + 180.0f * static_cast<float>(index / 4));
                auto wind = ecs.CreateEntity();
                wind.AddComponent<NameComponent>("BistroGrassWind" + std::to_string(index));
                wind.AddComponent<TransformComponent>(position);
                GDX12WindSettings windSettings;
                windSettings.Implementation = GDX12WindImplementation::Analytic;
                windSettings.ExtentsCm = Vector2(90, 70);
                windSettings.SpeedCmPerSecond = 45;
                windSettings.GustAmplitude = 0.25f;
                windSettings.GustFrequencyHz = 0.4f;
                wind.AddComponent<WindFieldComponent>().SetSettings(windSettings);

                auto grass = ecs.CreateEntity();
                grass.AddComponent<NameComponent>("BistroGrass" + std::to_string(index));
                grass.AddComponent<TransformComponent>(position);
                GDX12GrassSettings grassSettings;
                grassSettings.Implementation = GDX12GrassImplementation::CrossedRibbons;
                grassSettings.ExtentsCm = Vector2(55, 40);
                grassSettings.DensityPerSquareMetre = 1400;
                grassSettings.MaxBlades = 2048;
                grassSettings.Segments = 3;
                grassSettings.HeightCm = 30;
                grassSettings.WidthCm = 1.4f;
                grassSettings.MaxBendCm = 12;
                grassSettings.Seed = static_cast<UINT>(index + 1) * 7919;
                grassSettings.CaptureReflections = false;
                auto& component = grass.AddComponent<GrassRenderComponent>();
                component.SetSettings(grassSettings);
                component.Material = _material;
                component.WindEntity = wind.GetId();
                _patches.push_back({grass.GetId(), wind.GetId()});
            }
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
    };
}
