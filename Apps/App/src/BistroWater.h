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
    inline AutoConsoleVariable<int> Count(L"App.Water.Count", 1,
        L"Bistro water basins, 0 disables, maximum 8",
        EConsoleVariableFlags::Default, [](int value) { return value >= 0 && value <= 8; });

    class Scene
    {
    public:
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
            constexpr float referenceUnitCm = 20;
            settings.ParticleRadiusCm = 0.1f * referenceUnitCm;
            settings.SmoothingRadiusCm = 0.2f * referenceUnitCm;
            settings.ParticleMassKg = 0.0128f;
            settings.RestDensityKgPerCm3 = 630 * settings.ParticleMassKg /
                (referenceUnitCm * referenceUnitCm * referenceUnitCm);
            settings.AccelerationCmPerSecond2 = Vector3(0, -9.81f * referenceUnitCm, 0);
            settings.ContainerHalfExtentCm = HalfExtent;
            settings.MaxStepSeconds = 1.0f / 180;
            settings.MaxSubsteps = 3;
            settings.MaxSpeedCmPerSecond = 1000;
            settings.MaxAccelerationCmPerSecond2 = 100000;
            settings.PressureStiffnessCm2PerSecond2 = 288 * referenceUnitCm * referenceUnitCm;
            settings.NearPressureStiffnessCm2PerSecond2 = 2.15f * referenceUnitCm * referenceUnitCm;
            settings.PredictionSeconds = 1.0f / 120;
            settings.ViscosityCm2PerSecond = 0;
            settings.Restitution = 0.95f;
            settings.FilterIterations = 2;
            settings.FilterRadiusPixels = 6;
            settings.AbsorptionPerCm = {0.008f,0.002f,0.001f};
            settings.ScatteringColor = {0.02f,0.12f,0.16f};
            _source.SetSettings(settings);
            // MGPU uses a jittered volume lattice at rest, inside 90% of the container.
            constexpr float jitter = 0.1f * referenceUnitCm;
            const Vector3 center(0, HalfExtent.y * 0.1f, 0);
            auto available = HalfExtent - Vector3(settings.ParticleRadiusCm + jitter + 1);
            available.y -= center.y;
            const Vector3 size = available * 1.8f;
            const float scale = std::cbrt(50000.0f / (size.x * size.y * size.z));
            const UINT nx = (std::max)(1u, static_cast<UINT>(std::round(scale * size.x)));
            const UINT ny = (std::max)(1u, static_cast<UINT>(std::round(scale * size.y)));
            const UINT nz = (std::max)(1u, static_cast<UINT>(std::round(scale * size.z)));
            std::mt19937 random(0xB15720u);
            std::uniform_real_distribution<float> offset(-jitter, jitter);
            std::vector<GDX12FluidParticle> particles;
            particles.reserve(nx * ny * nz);
            for (UINT x = 0; x < nx; ++x)
                for (UINT y = 0; y < ny; ++y)
                    for (UINT z = 0; z < nz; ++z)
                    {
                        const Vector3 t(nx == 1 ? 0.5f : float(x) / (nx - 1),
                            ny == 1 ? 0.5f : float(y) / (ny - 1), nz == 1 ? 0.5f : float(z) / (nz - 1));
                        particles.push_back({center + (t - Vector3(0.5f)) * size +
                            Vector3(offset(random), offset(random), offset(random)), Vector3::Zero});
                    }
            _source.SetParticles(std::move(particles));
        }

        struct Basin { Entity Water; Entity Container; };
        std::vector<Basin> _basins;
        Engine::Core::MeshHandle _mesh;
        std::vector<GDX12Material*> _materials;
        GDX12FluidComponent _source;
    };
}
