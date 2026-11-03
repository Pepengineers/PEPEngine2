#pragma once

#include "App.Base/ECS/World.h"
#include "App.Base/ECS/WorldLoader.h"
#include "App.Base/Modules/RenderModule.h"
#include "App.Base/Systems/IndirectCommandBuildSystem.h"
#include "Engine.Core/AssetManager.h"
#include "Engine.Core/BenchmarkEngine.h"
#include "Common/ConsoleVariables.h"
#include <array>
#include <vector>

namespace BistroWater
{
    inline AutoConsoleVariable<int> Count(L"App.Water.Count", 1,
        L"Bistro water basins, 0 disables, maximum 8",
        EConsoleVariableFlags::Default, [](int value) { return value >= 0 && value <= 8; });

    class Scene
    {
    public:
        void Update(World& world, float)
        {
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
                // Exterior sidewalk is 23.162 cm high here; the basin base sits at 23.3 cm.
                const Vector3 position(-410.0f, 41.3f, -100.0f - 160.0f * static_cast<float>(index));
                auto basin = ecs.CreateEntity();
                basin.AddComponent<NameComponent>("BistroWaterBasin" + std::to_string(index));
                basin.AddComponent<TransformComponent>(position);
                basin.AddComponent<StaticMeshRenderComponent>(_mesh, _materials);
                auto water = ecs.CreateEntity();
                water.AddComponent<NameComponent>("BistroWater" + std::to_string(index));
                water.AddComponent<TransformComponent>(position);
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
            box({0,-16,0}, {52,2,52});
            box({-48,0,0}, {4,14,52});
            box({48,0,0}, {4,14,52});
            box({0,0,-48}, {44,14,4});
            box({0,0,48}, {44,14,4});
            part.Bounds = {{0,-2,0}, {52,16,52}};
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

            GDX12FluidSettings settings;
            settings.ParticleRadiusCm = 3;
            settings.SmoothingRadiusCm = 9;
            settings.ParticleMassKg = 0.125f;
            settings.ContainerHalfExtentCm = {44,14,44};
            settings.MaxSubsteps = 4;
            settings.MaxSpeedCmPerSecond = 180;
            settings.MaxAccelerationCmPerSecond2 = 3000;
            settings.PressureStiffnessCm2PerSecond2 = 10000;
            settings.ViscosityCm2PerSecond = 12;
            settings.Restitution = 0.05f;
            settings.FilterIterations = 2;
            settings.FilterRadiusPixels = 6;
            settings.AbsorptionPerCm = {0.008f,0.002f,0.001f};
            settings.ScatteringColor = {0.02f,0.12f,0.16f};
            _source.SetSettings(settings);
            std::vector<GDX12FluidParticle> particles;
            particles.reserve(512);
            for (UINT y = 0; y < 2; ++y)
                for (UINT z = 0; z < 16; ++z)
                    for (UINT x = 0; x < 16; ++x)
                        particles.push_back({{5.0f * x - 37.5f, 5.0f * y - 5.0f, 5.0f * z - 37.5f}, {}});
            _source.SetParticles(std::move(particles));
        }

        struct Basin { Entity Water; Entity Container; };
        std::vector<Basin> _basins;
        Engine::Core::MeshHandle _mesh;
        std::vector<GDX12Material*> _materials;
        GDX12FluidComponent _source;
    };
}
