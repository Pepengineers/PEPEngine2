#pragma once

#include "App.Base/ECS/World.h"
#include "Components/BistroWaterComponents.h"
#include "App.Base/ECS/WorldLoader.h"
#include "App.Base/Modules/RenderModule.h"
#include "App.Base/Systems/IndirectCommandBuildSystem.h"
#include "Engine.Core/AssetManager.h"
#include "Engine.Core/BenchmarkEngine.h"
#include "Common/ConsoleVariables.h"
#include "BistroWaterLayout.h"
#include <array>
#include <cmath>
#include <random>
#include <vector>

namespace BistroWater
{
    inline const Vector3 ReferenceHalfExtent(5, 1, 10);
    inline const Vector3 ReferenceScale(HalfExtent.x / ReferenceHalfExtent.x,
        HalfExtent.y / ReferenceHalfExtent.y, HalfExtent.z / ReferenceHalfExtent.z);
    inline AutoConsoleVariable<float> HeightScale(L"App.Water.HeightScale", 2.0f,
        L"Vertical Simulation-to-world scale, preserves basin floor, startup only, 1..3",
        EConsoleVariableFlags::ReadOnly, [](float value) { return std::isfinite(value) && value >= 1 && value <= 3; });
    // shortcut: virtual walls extend above the rim; use open-top collisions for actual overflow.
    inline Vector3 SimulationScale() { return ReferenceScale * Vector3(1, HeightScale.GetValue(), 1); }
    inline Vector3 SimulationOffset() { return Vector3(0, HalfExtent.y * (HeightScale.GetValue() - 1), 0); }
    inline AutoConsoleVariable<int> Count(L"App.Water.Count", 1,
        L"Bistro water basins, 0 disables, maximum 8",
        EConsoleVariableFlags::Default, [](int value) { return value >= 0 && value <= 8; });
    inline AutoConsoleVariable<float> WaveSpeed(L"App.Water.WaveSpeed", 6.0f,
        L"Initial mirrored wave speed away from the center in simulation units/s, startup only, 0..12",
        EConsoleVariableFlags::ReadOnly, [](float value) { return std::isfinite(value) && value >= 0 && value <= 12; });
    inline AutoConsoleVariable<float> WaveAcceleration(L"App.Water.WaveAcceleration", 4.0f,
        L"Periodic mirrored X wave acceleration in simulation units/s^2, startup only, 0..12",
        EConsoleVariableFlags::ReadOnly, [](float value) { return std::isfinite(value) && value >= 0 && value <= 12; });
    inline AutoConsoleVariable<float> WavePeriod(L"App.Water.WavePeriod", 6.0f,
        L"Wave forcing period in simulation seconds, startup only, 1..30",
        EConsoleVariableFlags::ReadOnly, [](float value) { return std::isfinite(value) && value >= 1 && value <= 30; });
    inline AutoConsoleVariable<int> ParticleCount(L"App.Water.ParticleCount", 100000,
        L"Fluid particle target, startup only, 10000..300000", EConsoleVariableFlags::ReadOnly,
        [](int value) { return value >= 10000 && value <= 300000; });
    inline AutoConsoleVariable<UINT> PresetParticleCount(L"App.Water.PresetParticleCount", 0,
        L"Fluid resolution target on world restart, 25000..100000; 0 uses the legacy target",
        EConsoleVariableFlags::Default, [](UINT value) { return value == 0 || (value >= 25000 && value <= 100000); });
    inline AutoConsoleVariable<UINT> FilterRadiusPixels(L"App.Water.FilterRadiusPixels", 6,
        L"Water surface filter radius on world restart, 1..64 pixels",
        EConsoleVariableFlags::Default, [](UINT value) { return value >= 1 && value <= 64; });
    inline AutoConsoleVariable<float> ParticleRadius(L"App.Water.ParticleRadius", 0.15f,
        L"Fluid render radius in simulation units, startup only, 0.01..0.2", EConsoleVariableFlags::ReadOnly,
        [](float value) { return std::isfinite(value) && value >= 0.01f && value <= 0.2f; });

    class Scene
    {
    public:
        void ResetWorld()
        {
            _basins.clear();
            if (_mesh.IsValid()) _source = CreateFluidSource();
        }

        void Update(World& world, float)
        {
            RegisterBistroWaterComponents(world);
            auto& ecs = world.GetECS();
            const size_t requested = static_cast<size_t>(Count.GetValue());
            while (_basins.size() > requested)
            {
                ecs.DestroyEntity(_basins.back().Water);
                ecs.DestroyEntity(_basins.back().Container);
                _basins.pop_back();
            }
            if (_basins.size() == requested) return;

            auto& render = *BenchmarkEngine::GetLocator().GetModule<RenderModule>();
            if (!_mesh.IsValid()) CreateResources(render);
            render.ReserveTransformCapacity(static_cast<UINT>(2 * (requested - _basins.size())));
            render.ReserveInstanceCapacity(static_cast<UINT>(requested - _basins.size()));
            while (_basins.size() < requested)
            {
                const size_t index = _basins.size();
                const auto offset = Vector3::TransformNormal(Vector3(0, 0,
                    -(2 * HalfExtent.z + 80) * static_cast<float>(index)),
                    Matrix::CreateRotationY(XMConvertToRadians(YawDegrees)));
                const auto position = Center + offset;
                auto basin = ecs.CreateEntity();
                basin.AddComponent<NameComponent>("BistroWaterBasin" + std::to_string(index));
                basin.AddComponent<TransformComponent>(position, Vector3(0, YawDegrees, 0));
                basin.AddComponent<StaticMeshRenderComponent>(_mesh, _materials);
                auto water = ecs.CreateEntity();
                water.AddComponent<NameComponent>("BistroWater" + std::to_string(index));
                water.AddComponent<TransformComponent>(position + SimulationOffset(), Vector3(0, YawDegrees, 0), SimulationScale());
                static_cast<GDX12FluidComponent&>(water.AddComponent<FluidRenderComponent>()) = _source;
                _basins.push_back({water.GetId(), basin.GetId()});
            }
            IndirectCommandBuildSystem{}.Tick(world, 0);
        }

    private:
        static std::unique_ptr<Engine::Core::Mesh> CreateBasinMesh()
        {
            Engine::Core::SubMesh part;
            const auto box = [&](Vector3 center, Vector3 extent)
            {
                const std::array<Vector3, 8> corners = {{
                    {-1,-1,-1}, {1,-1,-1}, {1,1,-1}, {-1,1,-1},
                    {-1,-1,1}, {1,-1,1}, {1,1,1}, {-1,1,1}}};
                constexpr UINT faces[6][4] = {{0,3,2,1}, {4,5,6,7}, {0,4,7,3},
                    {1,2,6,5}, {0,1,5,4}, {3,7,6,2}};
                const Vector3 normals[] = {{0,0,-1}, {0,0,1}, {-1,0,0}, {1,0,0}, {0,-1,0}, {0,1,0}};
                const Vector2 uvs[] = {{0,0}, {0,1}, {1,1}, {1,0}};
                for (UINT face = 0; face < 6; ++face)
                {
                    const UINT first = static_cast<UINT>(part.Vertices.size());
                    for (UINT corner = 0; corner < 4; ++corner)
                    {
                        Engine::Core::Vertex vertex;
                        vertex.Position = center + corners[faces[face][corner]] * extent;
                        vertex.PreviousPosition = vertex.Position;
                        vertex.Normal = normals[face];
                        vertex.TexCoord = uvs[corner];
                        part.Vertices.push_back(vertex);
                    }
                    for (UINT offset : {0u, 1u, 2u, 0u, 2u, 3u}) part.Indices.push_back(first + offset);
                }
            };
            box({0,-HalfExtent.y-2,0}, {HalfExtent.x+8,2,HalfExtent.z+8});
            box({-HalfExtent.x-4,0,0}, {4,HalfExtent.y,HalfExtent.z+8});
            box({HalfExtent.x+4,0,0}, {4,HalfExtent.y,HalfExtent.z+8});
            box({0,0,-HalfExtent.z-4}, {HalfExtent.x,HalfExtent.y,4});
            box({0,0,HalfExtent.z+4}, {HalfExtent.x,HalfExtent.y,4});
            part.Bounds = {{0,-2,0}, {HalfExtent.x+8,HalfExtent.y+2,HalfExtent.z+8}};
            part.SourceNodeName = "BistroWaterBasin";
            Engine::Core::MeshMaterial material;
            material.Name = "BistroWaterStone";
            material.DiffuseColor = {0.32f, 0.30f, 0.26f};
            material.Roughness = 0.85f;
            const auto bounds = part.Bounds;
            return std::make_unique<Engine::Core::Mesh>(
                std::vector<Engine::Core::SubMesh>{std::move(part)}, bounds,
                std::vector<Engine::Core::MeshMaterial>{std::move(material)});
        }

        void CreateResources(RenderModule& render)
        {
            auto& meshes = Engine::Core::AssetManager::GetInstance().Meshes();
            _mesh = meshes.Register(Engine::Core::MeshAssetLocator{
                std::filesystem::path(ASSETS_FOLDER) / "Models/BistroWaterBasin.generated"});
            if (!_mesh.IsValid()) throw std::runtime_error("Cannot register Bistro water basin");
            auto* mesh = meshes.GetMesh(_mesh);
            if (!mesh) mesh = meshes.Cache(_mesh, CreateBasinMesh());
            if (!mesh) throw std::runtime_error("Cannot create Bistro water basin mesh");
            render.SubmitMesh(mesh, _mesh);
            if (auto* material = render.GetMaterialByName("BistroWaterBasin_Material_0")) _materials = {material};
            else _materials = WorldLoader::BuildMaterialSlots(render, *mesh, "BistroWaterBasin");
            _source = CreateFluidSource();
        }

    public:
        static GDX12FluidComponent CreateFluidSource()
        {
            GDX12FluidSettings settings;
            // Keep solver numerical units; only the entity transform converts them to scene cm.
            settings.ReferenceModel = true;
            settings.ParticleRadiusCm = ParticleRadius.GetValue();
            settings.SmoothingRadiusCm = 0.2f;
            settings.ParticleMassKg = 1;
            settings.RestDensityKgPerCm3 = 630;
            settings.AccelerationCmPerSecond2 = Vector3(0, -9.81f, 0);
            settings.WaveAcceleration = WaveAcceleration.GetValue();
            settings.WavePeriodSeconds = WavePeriod.GetValue();
            settings.ContainerHalfExtentCm = ReferenceHalfExtent;
            settings.MaxStepSeconds = 1.0f / 180;
            settings.MaxSubsteps = 3;
            settings.PressureStiffnessCm2PerSecond2 = 288;
            settings.NearPressureStiffnessCm2PerSecond2 = 2.15f;
            settings.PredictionSeconds = 1.0f / 120;
            settings.ViscosityCm2PerSecond = 0;
            settings.Restitution = 0.95f;
            settings.FilterRadiusPixels = FilterRadiusPixels.GetValue();
            settings.AbsorptionPerCm = {0.02f,0.006f,0.0015f};
            settings.ScatteringColor = {0.015f,0.18f,0.45f};
            // Same two-pass spawn sizing as HybridParticleApp::RecreateFluidEmitter.
            const auto spawnSize = [&](UINT count)
            {
                const float width = count <= 50000 ?
                    5 + 5 * std::log10(float(count) / 10000) / std::log10(5.0f) :
                    10 + 10 * std::log10(float(count) / 50000) / std::log10(2.0f);
                return Vector3::Min(Vector3(std::clamp(width, 5.0f, 20.0f), 10, 20),
                    settings.ContainerHalfExtentCm * 1.8f);
            };
            const UINT preset = PresetParticleCount.GetValue();
            const UINT target = preset ? preset : static_cast<UINT>(ParticleCount.GetValue());
            const auto grid = [target](Vector3 size)
            {
                const float scale = std::cbrt(float(target) / (size.x * size.y * size.z));
                return std::array<UINT, 3>{
                    (std::max)(1u, static_cast<UINT>(std::round(scale * size.x))),
                    (std::max)(1u, static_cast<UINT>(std::round(scale * size.y))),
                    (std::max)(1u, static_cast<UINT>(std::round(scale * size.z)))};
            };
            const auto initial = grid(spawnSize(target));
            auto size = spawnSize(initial[0] * initial[1] * initial[2]);
            size.x = (std::min)(size.x, settings.ContainerHalfExtentCm.x);
            const auto counts = grid(size);
            const auto nx = counts[0] + (counts[0] & 1u), ny = counts[1], nz = counts[2];
            if (preset && preset < 100000)
            {
                // Preserve represented fluid mass and scale the kernel with average particle spacing.
                settings.ParticleMassKg = 100000.0f / static_cast<float>(nx * ny * nz);
                settings.SmoothingRadiusCm *= std::cbrt(settings.ParticleMassKg);
            }
            constexpr float jitter = 0.1f;
            const Vector3 center(0, settings.ContainerHalfExtentCm.y * 0.1f, 0);
            std::mt19937 random(0xB15720u);
            std::uniform_real_distribution<float> offset(-jitter, jitter);
            std::vector<GDX12FluidParticle> particles;
            particles.reserve(nx * ny * nz);
            for (UINT x = 0; x < nx / 2; ++x)
                for (UINT y = 0; y < ny; ++y)
                    for (UINT z = 0; z < nz; ++z)
                    {
                        const Vector3 t(nx == 1 ? 0.5f : float(x) / (nx - 1),
                            ny == 1 ? 0.5f : float(y) / (ny - 1), nz == 1 ? 0.5f : float(z) / (nz - 1));
                        auto position = center + (t - Vector3(0.5f)) * size +
                            Vector3(offset(random), offset(random), offset(random));
                        position.x = -std::abs(position.x);
                        particles.push_back({position, Vector3(-WaveSpeed.GetValue(), 0, 0)});
                        position.x = -position.x;
                        particles.push_back({position, Vector3(WaveSpeed.GetValue(), 0, 0)});
                    }
            GDX12FluidComponent source;
            source.SetSettings(settings);
            source.SetParticles(std::move(particles));
            return source;
        }

    private:
        struct Basin { Entity Water; Entity Container; };
        std::vector<Basin> _basins;
        Engine::Core::MeshHandle _mesh;
        std::vector<GDX12Material*> _materials;
        GDX12FluidComponent _source;
    };
}
