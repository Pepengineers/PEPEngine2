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
#include <vector>

namespace BistroWater
{
    inline AutoConsoleVariable<int> Count(L"App.Water.Count", 1,
        L"Bistro water basins, 0 disables, maximum 8",
        EConsoleVariableFlags::Default, [](int value) { return value >= 0 && value <= 8; });

    class Scene
    {
    public:
        void Update(World& world, float deltaSeconds)
        {
            RegisterBistroWaterComponents(world);
            auto& ecs = world.GetECS();
            if (!world.IsPaused() && deltaSeconds > 0 && world.GetTimeScale() > 0)
            {
                _waveTime += deltaSeconds * world.GetTimeScale();
                const auto rotation = Matrix::CreateRotationY(XMConvertToRadians(YawDegrees));
                for (const auto& basin : _basins)
                {
                    auto& fluid = ecs.Get<FluidRenderComponent>(basin.Water);
                    // Drive the SPH solver continuously; pressure and wall collisions form the waves.
                    fluid.SetAcceleration(Vector3::TransformNormal(
                        Vector3(90 * std::cos(_waveTime * 1.1f), -981, 65 * std::sin(_waveTime * 1.7f)), rotation));
                }
            }
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
                water.AddComponent<TransformComponent>(position, Vector3(0, YawDegrees, 0));
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

            GDX12FluidSettings settings;
            settings.ParticleRadiusCm = 18;
            settings.SmoothingRadiusCm = 54;
            settings.ParticleMassKg = 11.904f;
            settings.ContainerHalfExtentCm = HalfExtent;
            settings.MaxSubsteps = 32;
            settings.MaxSpeedCmPerSecond = 300;
            settings.MaxAccelerationCmPerSecond2 = 6000;
            settings.PressureStiffnessCm2PerSecond2 = 40000;
            settings.ViscosityCm2PerSecond = 12;
            settings.Restitution = 0.05f;
            settings.FilterIterations = 2;
            settings.FilterRadiusPixels = 6;
            settings.AbsorptionPerCm = {0.008f,0.002f,0.001f};
            settings.ScatteringColor = {0.02f,0.12f,0.16f};
            _source.SetSettings(settings);
            std::vector<GDX12FluidParticle> particles;
            particles.reserve(1024);
            for (UINT y = 0; y < 2; ++y)
                for (UINT z = 0; z < 32; ++z)
                    for (UINT x = 0; x < 16; ++x)
                        particles.push_back({{32.0f * x - 240, 24.0f * y - 19, 15.5f * z - 240.25f},
                            {35, 15 * std::sin(x * 0.4f + z * 0.3f), 20}});
            _source.SetParticles(std::move(particles));
        }

        struct Basin { Entity Water; Entity Container; };
        std::vector<Basin> _basins;
        Engine::Core::MeshHandle _mesh;
        std::vector<GDX12Material*> _materials;
        GDX12FluidComponent _source;
        float _waveTime = 0;
    };
}
