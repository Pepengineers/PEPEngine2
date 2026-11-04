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
#include <vector>

namespace BistroReflections
{
    inline AutoConsoleVariable<int> Count(L"App.ReflectionProbes.Count", 4,
        L"Frozen static Bistro cubemaps, 0 disables, maximum 32",
        EConsoleVariableFlags::Default, [](int value) { return value >= 0 && value <= static_cast<int>(MaxReflectionProbes); });
    inline AutoConsoleVariable<bool> ShowBounds(L"App.ReflectionProbes.ShowBounds", false,
        L"Show colored table-sized reflection boxes and capture labels above Bistro terrace tables");
    inline AutoConsoleVariable<bool> ShowSpheres(L"App.ReflectionProbes.ShowSpheres", true,
        L"Show mirror spheres sampling each Bistro probe's own baked cubemap");

    struct TableAnchor { Vector3 Centre; float HalfSize; };
    // Exterior OBJ table bounds, ordered along the entrance terrace first.
    inline const std::array<TableAnchor, 11> Tables = {{
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
        {{748.304f,107.503f,677.719f},58.398f}}};

    class Scene
    {
    public:
        void Update(World& world, float)
        {
            auto& ecs = world.GetECS();
            const size_t requested = static_cast<size_t>(Count.GetValue());
            const bool visible = ShowSpheres.GetValue();
            if (_entities.size() == requested && _visible == visible) return;
            if (requested && !_mesh.IsValid()) CreateMesh();
            while (_entities.size() > requested)
            {
                ecs.DestroyEntity(_entities.back());
                _entities.pop_back();
            }
            while (_entities.size() < requested)
            {
                const size_t index = _entities.size();
                const auto& table = Tables[index % Tables.size()];
                // Include the tabletop in the blend region; additional layers retain the existing 32-probe limit.
                const Vector3 position = table.Centre + Vector3(0,
                    table.HalfSize - 5 + 2 * table.HalfSize * static_cast<float>(index / Tables.size()), 0);
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
            auto& render = *BenchmarkEngine::GetLocator().GetModule<RenderModule>();
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
                const auto label = "Baked probe " + std::to_string(index + 1);
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
        Engine::Core::MeshHandle _mesh;
        bool _visible = true;
    };
}
