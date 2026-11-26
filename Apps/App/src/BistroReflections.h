#pragma once

#include "App.Base/ECS/World.h"
#include "BistroProbeSettings.h"
#include "Common/ConsoleVariables.h"
#include "App.Base/Modules/RenderModule.h"
#include "Engine.UI/UI_test.h"
#include "Engine.Core/AssetManager.h"
#include "App.Base/Systems/IndirectCommandBuildSystem.h"
#include <directxtk/GeometricPrimitive.h>
#include <algorithm>
#include <array>
#include <vector>
#include "Engine.Core/Logging/Logger.h"

namespace BistroReflections
{
    inline AutoConsoleVariable<bool> ShowBounds(L"Render.ReflectionProbes.ShowBounds", false,
        L"Show colored table-sized reflection boxes and capture labels above Bistro tables");
    inline AutoConsoleVariable<UINT> Resolution(L"Render.ReflectionProbes.Resolution", 128,
        L"Capture face size, a power of two in 16..512", EConsoleVariableFlags::Default,
        [](UINT value) { return value >= 16 && value <= 512 && (value & (value - 1)) == 0; });
    inline AutoConsoleVariable<UINT> PrefilterSamples(L"Render.ReflectionProbes.PrefilterSamples", 64,
        L"Capture prefilter samples, 8..1024", EConsoleVariableFlags::Default,
        [](UINT value) { return value >= 8 && value <= 1024; });
    inline AutoConsoleVariable<int> Implementation(L"Render.ReflectionProbes.Implementation", 2,
        L"0 full dynamic, 1 baked static with dynamic overlay, 2 frozen manual capture", EConsoleVariableFlags::Default,
        [](int value) { return value >= 0 && value <= 2; });
    inline AutoConsoleVariable<UINT> FirstFacesPerFrame(L"Render.ReflectionProbes.First.FacesPerFrame", 6,
        L"Manual face budget for the first Bistro probe at default Epic UpdateFrequency with quality preset disabled",
        EConsoleVariableFlags::Default,
        [](UINT value) { return value >= 1 && value <= 6; });
    inline AutoConsoleVariable<int> FirstImplementation(L"Render.ReflectionProbes.First.Implementation", -1,
        L"Capture mode of the first probe; -1 inherits Implementation", EConsoleVariableFlags::Default,
        [](int value) { return value >= -1 && value <= 2; });

    struct TableAnchor { Vector3 Centre; float HalfSize; };
    // Tabletop centres from the Paris_Table_01 groups in Exterior/exterior.obj.
    inline const std::array<TableAnchor, 11> TerraceTables = {{
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
    static_assert(TerraceTables.size() <= MaxReflectionProbes);
    inline constexpr float SphereRadius = 24.f;
    inline AutoConsoleVariable<int> Count(L"Render.ReflectionProbes.Count", 0,
        L"Terrace table probes in manual mode; 0 follows reflection quality, maximum " + std::to_wstring(TerraceTables.size()),
        EConsoleVariableFlags::Default, [](int value) { return value >= 0 && value <= static_cast<int>(TerraceTables.size()); });

    inline AutoConsoleVariable<UINT> Recapture(L"Render.ReflectionProbes.Recapture", 0,
        L"Increment to explicitly recapture frozen probes");

    class Scene
    {
    public:
        void Update(World& world, float)
        {
            auto& ecs = world.GetECS();
            const bool preset = UseQualityPreset.GetValue();
            const auto settings = GetReflectionSettings();
            const int manualCount = Count.GetValue();
            const int activeCount = preset || manualCount == 0 ? settings.Quality : manualCount;
            const size_t requested = settings.Quality == 0 ? 0 : static_cast<size_t>(activeCount);
            const bool recapture = _recapture != Recapture.GetValue();
            const auto resolution = preset ? Side(settings.Quality) : Resolution.GetValue();
            const auto samples = preset ? Samples(settings.Quality) : PrefilterSamples.GetValue();
            const auto implementation = Implementation.GetValue();
            std::array<int, AdapterNames.size()> adapters{};
            for (size_t i = 0; i < adapters.size(); ++i) adapters[i] = Adapters[i]->GetValue();
            const auto firstFaces = FirstFacesPerFrame.GetValue();
            const auto firstImplementation = FirstImplementation.GetValue();
            if (_activeCount == requested &&
                _resolution == resolution && _samples == samples && _implementation == implementation &&
                _adapters == adapters && _firstFaces == firstFaces && _firstImplementation == firstImplementation &&
                !recapture) return;
            auto& render = *BenchmarkEngine::GetLocator().GetModule<RenderModule>();
            if (_activeCount != requested)
            {
                render.WaitForUploads();
                if (requested > _entities.size())
                {
                    render.ReserveTransformCapacity(static_cast<UINT>(requested - _entities.size()));
                    render.ReserveInstanceCapacity(static_cast<UINT>(requested - _entities.size()));
                }
            }
            if (requested && !_mesh.IsValid()) CreateMesh();
            while (_entities.size() < requested)
            {
                const size_t index = _entities.size();
                const auto& table = TerraceTables[index];
                const Vector3 position = table.Centre + Vector3(0, SphereRadius, 0);
                auto entity = ecs.CreateEntity();
                entity.AddComponent<NameComponent>("BistroReflection" + std::to_string(index));
                entity.AddComponent<TransformComponent>(position);
                auto& probe = entity.AddComponent<ReflectionProbeComponent>();
                probe.Implementation = index == 0 && firstImplementation >= 0 ? firstImplementation : implementation;
                probe.Resolution = resolution;
                probe.PrefilterSamples = samples;
                probe.Extents = Vector3(table.HalfSize);
                probe.BlendDistance = 5;
                probe.FacesPerFrame = index == 0 ? firstFaces : 6;
                probe.BistroIndex = static_cast<UINT>(index);
                if (index < adapters.size()) probe.Adapter = adapters[index];
                probe.FarPlane = 15000;
                _entities.push_back(entity.GetId());
                Log::Info("BistroProbes", "terrace table=", index + 1,
                    "; origin=", position.x, ",", position.y, ",", position.z);
            }
            auto order = std::vector<Entity>(_entities.begin(), _entities.begin() + requested);
            std::sort(order.begin(), order.end()); // Same entity order as the published cubemap array.
            for (size_t index = 0; index < _entities.size(); ++index)
            {
                const auto entity = _entities[index];
                auto& probe = ecs.Get<ReflectionProbeComponent>(entity);
                const bool active = index < requested;
                probe.Enabled = active;
                if (!active)
                {
                    ecs.Get<StaticMeshRenderComponent>(entity).Visible = false;
                    continue;
                }
                if (recapture) probe.Invalidate();
                probe.Resolution = resolution;
                probe.PrefilterSamples = samples;
                if (index < adapters.size()) probe.Adapter = adapters[index];
                if (index == 0)
                {
                    probe.FacesPerFrame = firstFaces;
                    probe.Implementation = firstImplementation >= 0 ? firstImplementation : implementation;
                }
                else probe.Implementation = implementation;
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
                    transform.Scale = Vector3(SphereRadius);
                    transform.DirtyFlag = true;
                    ecs.GetEntityHandle(entity).AddComponent<StaticMeshRenderComponent>(_mesh, std::vector<GDX12Material*>{material});
                }
                auto& renderer = ecs.Get<StaticMeshRenderComponent>(entity);
                renderer.CastsShadows = false;
                renderer.Visible = true;
            }
            _activeCount = requested;
            _recapture = Recapture.GetValue();
            _resolution = resolution;
            _samples = samples;
            _implementation = implementation;
            _adapters = adapters;
            _firstFaces = firstFaces;
            _firstImplementation = firstImplementation;
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
            for (size_t index = 0; index < _activeCount; ++index)
            {
                const auto entity = _entities[index];
                const auto& position = ecs.Get<TransformComponent>(entity).Location;
                const auto& probe = ecs.Get<ReflectionProbeComponent>(entity);
                const auto label = "Terrace table capture " + std::to_string(index + 1);
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

        UINT _recapture = 0;
        std::vector<Entity> _entities;
        size_t _activeCount = 0;
        Engine::Core::MeshHandle _mesh;
        UINT _resolution = Resolution.GetValue();
        UINT _samples = PrefilterSamples.GetValue();
        int _implementation = -1;
        std::array<int, AdapterNames.size()> _adapters{};
        int _firstImplementation = -1;
        UINT _firstFaces = 6;
    };
}
