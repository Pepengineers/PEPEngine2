#pragma once

#include "App.Base/ECS/World.h"
#include "Common/ConsoleVariables.h"
#include "App.Base/Modules/RenderModule.h"
#include "Engine.UI/UI_test.h"
#include "Engine.Core/AssetManager.h"
#include "App.Base/Systems/IndirectCommandBuildSystem.h"
#include <directxtk/GeometricPrimitive.h>
#include <algorithm>
#include <array>
#include <numeric>
#include <random>
#include <vector>

namespace BistroReflections
{
    inline AutoConsoleVariable<bool> ShowBounds(L"App.ReflectionProbes.ShowBounds", false,
        L"Show colored table-sized reflection boxes and capture labels above Bistro tables");
    inline AutoConsoleVariable<bool> ShowSpheres(L"App.ReflectionProbes.ShowSpheres", false,
        L"Show mirror spheres sampling each Bistro probe's own baked cubemap");

    struct TableAnchor { Vector3 Centre; float HalfSize; };
    // OBJ tabletop centres: 11 terrace tables, then all 10 interior tables.
    inline const std::array<TableAnchor, 21> Tables = {{
        {{-337.744f,107.503f,-534.7695f},62.4795f},
        {{-132.7655f,107.503f,-482.213f},60.551f},
        {{2.9765f,107.503f,-739.244f},60.551f},
        {{135.6185f,107.503f,-990.404f},60.551f},
        {{-40.9755f,107.503f,356.486f},58.398f},
        {{153.2725f,107.503f,246.2075f},58.1845f},
        {{239.938f,107.503f,470.8165f},58.398f},
        {{429.8935f,107.503f,356.0345f},58.1845f},
        {{509.407f,107.503f,580.489f},58.398f},
        {{708.2035f,107.503f,466.5315f},58.1845f},
        {{748.304f,107.503f,677.719f},58.398f},
        {{963.496f,124.338f,173.940f},82.728f},
        {{32.347f,124.327f,-394.740f},82.728f},
        {{173.030f,124.327f,-648.631f},82.727f},
        {{222.673f,124.547f,83.986f},82.303f},
        {{501.667f,124.547f,192.991f},82.303f},
        {{779.009f,124.541f,302.068f},82.303f},
        {{790.023f,124.327f,76.496f},62.810f},
        {{490.576f,124.247f,-87.531f},62.810f},
        {{343.798f,124.219f,-164.417f},62.810f},
        {{638.546f,124.309f,-5.547f},62.810f}}};
    static_assert(Tables.size() <= MaxReflectionProbes);
    inline AutoConsoleVariable<int> Count(L"App.ReflectionProbes.Count", 4,
        L"Frozen static captures at random unique tables; 0 disables, maximum " +
        std::to_wstring(Tables.size()) + L" (the number of tables: 11 terrace + 10 interior)",
        EConsoleVariableFlags::Default, [](int value) { return value >= 0 && value <= static_cast<int>(Tables.size()); });

    class Scene
    {
    public:
        Scene() { std::iota(_tableOrder.begin(), _tableOrder.end(), size_t{0}); }

        void Update(World& world, float)
        {
            auto& ecs = world.GetECS();
            const size_t requested = static_cast<size_t>(Count.GetValue());
            const bool visible = ShowSpheres.GetValue();
            if (_entities.size() == requested && _visible == visible) return;
            auto& render = *BenchmarkEngine::GetLocator().GetModule<RenderModule>();
            if (_entities.size() != requested)
            {
                render.WaitForUploads();
                if (requested > _entities.size())
                {
                    render.ReserveTransformCapacity(static_cast<UINT>(requested - _entities.size()));
                    render.ReserveInstanceCapacity(static_cast<UINT>(requested - _entities.size()));
                }
                if (_entities.empty()) std::shuffle(_tableOrder.begin(), _tableOrder.end(), _random);
            }
            if (requested && !_mesh.IsValid()) CreateMesh();
            while (_entities.size() > requested)
            {
                ecs.DestroyEntity(_entities.back());
                _entities.pop_back();
            }
            while (_entities.size() < requested)
            {
                const size_t index = _entities.size();
                const auto& table = Tables[_tableOrder[index]];
                const Vector3 position = table.Centre + Vector3(0, table.HalfSize - 5, 0);
                auto entity = ecs.CreateEntity();
                entity.AddComponent<NameComponent>("BistroReflection" + std::to_string(index));
                entity.AddComponent<TransformComponent>(position);
                auto& probe = entity.AddComponent<ReflectionProbeComponent>();
                probe.Implementation = 2;
                probe.Extents = Vector3(table.HalfSize);
                probe.BlendDistance = 5;
                probe.Resolution = 128;
                probe.FacesPerFrame = 6;
                probe.PrefilterSamples = 64;
                probe.FarPlane = 15000;
                _entities.push_back(entity.GetId());
            }
            auto order = _entities;
            std::sort(order.begin(), order.end()); // Same entity order as the published cubemap array.
            for (size_t index = 0; index < _entities.size(); ++index)
            {
                const auto entity = _entities[index];
                const auto name = "BistroProbePreview" + std::to_string(index);
                auto* material = render.GetMaterialByName(name);
                if (!material) material = render.CreateMaterial(name);
                const UINT slot = static_cast<UINT>(std::lower_bound(order.begin(), order.end(), entity) - order.begin()) + 1;
                if (material->ReflectionProbePreview != slot)
                {
                    material->ReflectionProbePreview = slot;
                    material->DirtyFlag = true;
                }
                if (!ecs.Has<StaticMeshRenderComponent>(entity))
                {
                    auto& transform = ecs.Get<TransformComponent>(entity);
                    transform.Scale = Vector3(0.8f * ecs.Get<ReflectionProbeComponent>(entity).Extents.x);
                    transform.DirtyFlag = true;
                    ecs.GetEntityHandle(entity).AddComponent<StaticMeshRenderComponent>(_mesh, std::vector<GDX12Material*>{material});
                }
                ecs.Get<StaticMeshRenderComponent>(entity).Visible = visible;
            }
            _visible = visible;
            IndirectCommandBuildSystem{}.Tick(world, 0);
        }

        void DrawBounds(World& world, RenderModule& render) const
        {
            if (!ShowBounds.GetValue()) return;
            const auto* common = render.GetRenderPipelineCommonData();
            XMFLOAT4X4 viewProjection;
            XMStoreFloat4x4(&viewProjection, common->CameraView * common->CameraProjection);
            constexpr unsigned colors[] = {0xff40e8ff,0xff60ff60,0xffff70ff,0xffffb050};
            auto& ecs = world.GetECS();
            for (size_t index = 0; index < _entities.size(); ++index)
            {
                const auto entity = _entities[index];
                const auto& position = ecs.Get<TransformComponent>(entity).Location;
                const auto& probe = ecs.Get<ReflectionProbeComponent>(entity);
                const auto label = "Table capture " + std::to_string(_tableOrder[index] + 1);
                Engine::UI::DrawWorldBox({position,probe.Extents}, viewProjection, label.c_str(), colors[index % 4]);
            }
        }

    private:
        void CreateMesh()
        {
            auto& meshes = Engine::Core::AssetManager::GetInstance().Meshes();
            _mesh = meshes.Register(Engine::Core::MeshAssetLocator{
                std::filesystem::path(ASSETS_FOLDER) / "Models/ProbePreviewSphere.generated"});
            auto* mesh = meshes.GetMesh(_mesh);
            if (!mesh)
            {
                DirectX::GeometricPrimitive::VertexCollection vertices;
                DirectX::GeometricPrimitive::IndexCollection indices;
                DirectX::GeometricPrimitive::CreateSphere(vertices, indices, 2, 24);
                Engine::Core::SubMesh part;
                for (const auto& source : vertices)
                {
                    Engine::Core::Vertex vertex;
                    vertex.Position = vertex.PreviousPosition = source.position;
                    vertex.Normal = source.normal;
                    vertex.TexCoord = source.textureCoordinate;
                    part.Vertices.push_back(vertex);
                }
                part.Indices.assign(indices.begin(), indices.end());
                // DirectXTK's default winding is clockwise; this renderer uses CCW fronts.
                for (size_t i = 0; i < part.Indices.size(); i += 3) std::swap(part.Indices[i+1], part.Indices[i+2]);
                const BoundingBox bounds({0,0,0}, {1,1,1});
                part.Bounds = bounds;
                mesh = meshes.Cache(_mesh, std::make_unique<Engine::Core::Mesh>(
                    std::vector<Engine::Core::SubMesh>{std::move(part)}, bounds,
                    std::vector<Engine::Core::MeshMaterial>{Engine::Core::MeshMaterial{}}));
            }
            BenchmarkEngine::GetLocator().GetModule<RenderModule>()->SubmitMesh(mesh, _mesh);
        }

        std::vector<Entity> _entities;
        std::array<size_t, Tables.size()> _tableOrder;
        std::mt19937 _random{std::random_device{}()};
        Engine::Core::MeshHandle _mesh;
        bool _visible = true;
    };
}
