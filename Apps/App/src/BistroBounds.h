#pragma once
#include "Components/BistroGrassComponents.h"
#include "Components/BistroWaterComponents.h"

#include "BistroScene.h"
#include "App.Base/ECS/World.h"
#include "App.Base/Modules/RenderModule.h"
#include "Engine.Core/AssetManager.h"
#include "Engine.UI/UI_test.h"
#include "Common/ConsoleVariables.h"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <memory>
#include <vector>

namespace BistroBounds
{
    inline AutoConsoleVariable<bool> Show(L"App.Bounds.Show", false,
        L"Show current scene bounds; authored confined effects retain editable layout volumes. Session only.");
    inline constexpr float InteriorYawDegrees = -28.5f;

    struct Box
    {
    private:
        BoundingBox _bounds;
        BoundingBox _localBounds;
        bool _tracked;

    public:
        Box(const std::string& name, const BoundingBox& bounds, unsigned color, float yawDegrees = 0, bool tracked = false)
            : _bounds(bounds.Center, Vector3::Max(Vector3(0.01f), Vector3(bounds.Extents))), _localBounds(Vector3::Zero, bounds.Extents),
              _tracked(tracked), Name(name), Color(color),
              CenterX(Prefix() + L"CenterX", _bounds.Center.x, L"Bounds centre X, world cm", tracked ? EConsoleVariableFlags::ReadOnly : EConsoleVariableFlags::Default),
              CenterY(Prefix() + L"CenterY", _bounds.Center.y, L"Bounds centre Y, world cm", tracked ? EConsoleVariableFlags::ReadOnly : EConsoleVariableFlags::Default),
              CenterZ(Prefix() + L"CenterZ", _bounds.Center.z, L"Bounds centre Z, world cm", tracked ? EConsoleVariableFlags::ReadOnly : EConsoleVariableFlags::Default),
              HalfExtentX(Prefix() + L"HalfExtentX", _bounds.Extents.x, L"Bounds half-size X, cm", tracked ? EConsoleVariableFlags::ReadOnly : EConsoleVariableFlags::Default, Positive),
              HalfExtentY(Prefix() + L"HalfExtentY", _bounds.Extents.y, L"Bounds half-size Y, cm", tracked ? EConsoleVariableFlags::ReadOnly : EConsoleVariableFlags::Default, Positive),
              HalfExtentZ(Prefix() + L"HalfExtentZ", _bounds.Extents.z, L"Bounds half-size Z, cm", tracked ? EConsoleVariableFlags::ReadOnly : EConsoleVariableFlags::Default, Positive),
              Visible(Prefix() + L"Visible", true, L"Show this bounds box; does not disable its object"), _yawDegrees(yawDegrees)
        {}

        BoundingBox GetBounds() const
        {
            return {{CenterX.GetValue(), CenterY.GetValue(), CenterZ.GetValue()},
                {HalfExtentX.GetValue(), HalfExtentY.GetValue(), HalfExtentZ.GetValue()}};
        }
        float GetYawDegrees() const { return _yawDegrees; }

        const BoundingBox& GetLocalBounds() const { return _localBounds; }

        void Follow(const BoundingBox& local, const Matrix& world)
        {
            _localBounds = local;
            _localBounds.Extents = Vector3::Max(Vector3(0.01f), Vector3(local.Extents));
            if (_tracked) _localBounds.Transform(_bounds, world);
            else _bounds.Center = Vector3::Transform(Vector3(local.Center), world);
        }

        void Draw(const XMFLOAT4X4& viewProjection, const Matrix& world) const
        {
            XMFLOAT4X4 localViewProjection;
            XMStoreFloat4x4(&localViewProjection, world * XMLoadFloat4x4(&viewProjection));
            Engine::UI::DrawWorldBox(_localBounds, localViewProjection, Name.c_str(), Color);
        }

        const std::string Name;
        const unsigned Color;
        AutoConsoleVariableRef<float> CenterX, CenterY, CenterZ, HalfExtentX, HalfExtentY, HalfExtentZ;
        AutoConsoleVariable<bool> Visible;

    private:
        float _yawDegrees;
        std::wstring Prefix() const { return L"App.Bounds." + std::wstring(Name.begin(), Name.end()) + L"."; }
        static bool Positive(float value) { return value >= 0.01f && value <= 1000000; }
    };

    class Scene
    {
    public:
        explicit Scene(World& world)
        {
            RegisterBistroGrassComponents(world);
            RegisterBistroWaterComponents(world);
            auto& ecs = world.GetECS();
            const auto add = [&](Entity entity, const BoundingBox& local, unsigned color)
            {
                BoundingBox bounds;
                local.Transform(bounds, WorldMatrix(ecs.Get<TransformComponent>(entity)));
                auto box = std::make_unique<Box>(ecs.Get<NameComponent>(entity).value, bounds, color, 0, true);
                box->Follow(local, WorldMatrix(ecs.Get<TransformComponent>(entity)));
                _tracked.emplace_back(entity, box.get());
                _boxes.push_back(std::move(box));
            };
            ecs.ForEach<ParticleEmitterComponent, TransformComponent, NameComponent>(
                [&](Entity entity, const ParticleEmitterComponent& emitter, const TransformComponent&, const NameComponent& name)
                {
                    if (name.value == "BistroInteriorDust" || name.value == "BistroTableSteam" || name.value == "BistroPlanterFireflies")
                    {
                        const bool swarm = name.value == "BistroPlanterFireflies";
                        const auto bounds = name.value == "BistroInteriorDust" ?
                            BoundingBox({559,221,-348}, {694,181,779}) : swarm ? BistroArch::SwarmBounds : BoundingBox({922,227,-293}, {305,175,85.5f});
                        auto box = std::make_unique<Box>(name.value, bounds, 0xff40e8ff, swarm ? BistroArch::YawDegrees : InteriorYawDegrees);
                        _particleVolumes.push_back({entity, box.get(), bounds.Center});
                        _boxes.push_back(std::move(box));
                        return;
                    }
                    const auto& settings = emitter.GetSettings();
                    const auto extents = settings.VolumeEnabled ? settings.VolumeHalfExtents :
                        settings.Shape == ParticleShape::Point ? Vector3::Zero : settings.ShapeExtents;
                    add(entity, {Vector3::Zero, extents}, 0xff40e8ff);
                });
            ecs.ForEach<GrassRenderComponent, TransformComponent, NameComponent>(
                [&](Entity entity, const GrassRenderComponent& grass, const TransformComponent&, const NameComponent&)
                { add(entity, GrassLocalBounds(grass), 0xff60ff60); });
            ecs.ForEach<FluidRenderComponent, TransformComponent, NameComponent>(
                [&](Entity entity, const FluidRenderComponent& fluid, const TransformComponent&, const NameComponent&)
                { add(entity, {Vector3::Zero, fluid.GetSettings().ContainerHalfExtentCm}, 0xffffb050); });
            ecs.ForEach<SkeletalMeshRenderComponent, TransformComponent, NameComponent>(
                [&](Entity entity, const SkeletalMeshRenderComponent& mesh, const TransformComponent&, const NameComponent&)
                { add(entity, CharacterBounds(mesh), 0xffff70ff); });
        }

        void UpdateEmitters(World& world)
        {
            auto& ecs = world.GetECS();
            for (auto& volume : _particleVolumes)
            {
                const auto entity = volume.Owner;
                auto* box = volume.Bounds;
                if (!IsOwner(ecs, entity, *box) || !ecs.Has<ParticleEmitterComponent>(entity) ||
                    !ecs.Has<BistroParticles::EffectComponent>(entity)) continue;
                auto& emitter = ecs.Get<ParticleEmitterComponent>(entity);
                auto& effect = ecs.Get<BistroParticles::EffectComponent>(entity);
                auto& transform = ecs.Get<TransformComponent>(entity);
                const auto bounds = box->GetBounds();
                if (!volume.Initialized)
                {
                    transform.Location = bounds.Center;
                    transform.Rotation = {0, box->GetYawDegrees(), 0};
                    transform.Scale = Vector3::One;
                    transform.DirtyFlag = true;
                }
                else if (Vector3(bounds.Center) != volume.PreviousCenter)
                {
                    transform.Location = bounds.Center;
                    transform.DirtyFlag = true;
                }
                const auto worldMatrix = WorldMatrix(transform);
                if (!volume.Initialized || worldMatrix != volume.PreviousWorld) emitter.Reset();
                volume.PreviousCenter = transform.Location;
                volume.PreviousWorld = worldMatrix;
                volume.Initialized = true;
                box->Follow({Vector3::Zero, bounds.Extents}, worldMatrix);
                const auto& settings = emitter.GetSettings();
                // Reserve the billboard half-diagonal, so its corners stay inside the visible box too.
                const float radius = (std::max)(settings.SizeStart, settings.SizeEnd) *
                    (1 + settings.SizeVariation) * settings.RenderScale * 0.707107f;
                effect.LayoutVolume = true;
                const Vector3 scale(std::abs(transform.Scale.x), std::abs(transform.Scale.y), std::abs(transform.Scale.z));
                effect.AuthoredExtents = Vector3::Max(Vector3(0.01f), Vector3(bounds.Extents) -
                    Vector3(radius) / Vector3::Max(Vector3(0.00001f), scale));
            }
        }

        void DrawBounds(World& world, RenderModule& render)
        {
            auto& ecs = world.GetECS();
            for (const auto& [entity, box] : _tracked)
            {
                if (!IsOwner(ecs, entity, *box)) continue;
                auto local = box->GetLocalBounds();
                if (ecs.Has<ParticleEmitterComponent>(entity))
                {
                    const auto& settings = ecs.Get<ParticleEmitterComponent>(entity).GetSettings();
                    local = {Vector3::Zero, settings.VolumeEnabled ? settings.VolumeHalfExtents :
                        settings.Shape == ParticleShape::Point ? Vector3::Zero : settings.ShapeExtents};
                }
                else if (ecs.Has<GrassRenderComponent>(entity)) local = GrassLocalBounds(ecs.Get<GrassRenderComponent>(entity));
                else if (ecs.Has<FluidRenderComponent>(entity))
                    local = {Vector3::Zero, ecs.Get<FluidRenderComponent>(entity).GetSettings().ContainerHalfExtentCm};
                box->Follow(local, WorldMatrix(ecs.Get<TransformComponent>(entity)));
            }
            for (auto& volume : _particleVolumes)
            {
                if (!IsOwner(ecs, volume.Owner, *volume.Bounds) || !ecs.Has<ParticleEmitterComponent>(volume.Owner)) continue;
                const auto& transform = ecs.Get<TransformComponent>(volume.Owner);
                volume.Bounds->Follow({Vector3::Zero, volume.Bounds->GetBounds().Extents}, WorldMatrix(transform));
                volume.PreviousCenter = transform.Location;
            }
            if (!Show.GetValue()) return;
            const auto* common = render.GetRenderPipelineCommonData();
            XMFLOAT4X4 viewProjection;
            XMStoreFloat4x4(&viewProjection, common->CameraView * common->CameraProjection);
            for (const auto& [entity, box] : _tracked)
                if (IsOwner(ecs, entity, *box) && box->Visible.GetValue())
                    box->Draw(viewProjection, WorldMatrix(ecs.Get<TransformComponent>(entity)));
            for (const auto& volume : _particleVolumes)
                if (IsOwner(ecs, volume.Owner, *volume.Bounds) && ecs.Has<ParticleEmitterComponent>(volume.Owner) &&
                    volume.Bounds->Visible.GetValue())
                    volume.Bounds->Draw(viewProjection, WorldMatrix(ecs.Get<TransformComponent>(volume.Owner)));
        }

    private:
        static bool IsOwner(WorldECS& ecs, Entity entity, const Box& box)
        {
            return ecs.IsAlive(entity) && ecs.Has<TransformComponent>(entity) && ecs.Has<NameComponent>(entity) &&
                ecs.Get<NameComponent>(entity).value == box.Name;
        }

        static Matrix WorldMatrix(const TransformComponent& transform)
        {
            return Matrix::CreateScale(transform.Scale) * Matrix::CreateFromYawPitchRoll(
                XMConvertToRadians(transform.Rotation.y), XMConvertToRadians(transform.Rotation.x),
                XMConvertToRadians(transform.Rotation.z)) * Matrix::CreateTranslation(transform.Location);
        }

        // shortcut: character shape uses the startup pose; use animated local bounds when pose-tight boxes are needed.
        static BoundingBox CharacterBounds(const SkeletalMeshRenderComponent& component)
        {
            auto& assets = Engine::Core::AssetManager::GetInstance();
            const auto* scene = assets.Scenes().GetScene(component.GetScene());
            if (!scene) throw std::runtime_error("Cannot snapshot bounds of an unloaded character");
            const auto pose = scene->EvaluatePose(component.GetClip(), component.GetTimeSeconds(),
                component.IsLooping(), component.GetLocalPose());
            const auto& nodes = scene->GetNodes();
            Vector3 low(FLT_MAX), high(-FLT_MAX);
            bool found = false;
            for (size_t node = 0; node < nodes.size(); ++node)
                for (const auto handle : nodes[node].Meshes)
                {
                    const auto* mesh = assets.Meshes().GetMesh(handle);
                    if (!mesh) throw std::runtime_error("Cannot snapshot bounds of an unloaded character mesh");
                    for (const auto& part : mesh->GetSubMeshes())
                    {
                        std::vector<Matrix> palette;
                        for (const auto& bone : part.Bones)
                        {
                            const auto it = std::find_if(nodes.begin(), nodes.end(),
                                [&](const auto& candidate) { return candidate.Name == bone.NodeName; });
                            if (it == nodes.end()) throw std::runtime_error("Character bounds bone node is missing");
                            palette.push_back(bone.InverseBind * pose.at(it - nodes.begin()));
                        }
                        for (size_t vertex = 0; vertex < part.Vertices.size(); ++vertex)
                        {
                            const auto source = part.Vertices[vertex].Position;
                            Vector3 position = Vector3::Transform(source, pose.at(node));
                            if (!part.SkinVertices.empty() && part.SkinVertices.at(vertex).Count)
                            {
                                position = Vector3::Zero;
                                const auto range = part.SkinVertices.at(vertex);
                                for (UINT i = 0; i < range.Count; ++i)
                                {
                                    const auto influence = part.Influences.at(range.Offset + i);
                                    position += Vector3::Transform(source, palette.at(influence.BoneIndex)) * influence.Weight;
                                }
                            }
                            low = Vector3::Min(low, position);
                            high = Vector3::Max(high, position);
                            found = true;
                        }
                    }
                }
            if (!found) throw std::runtime_error("Character bounds have no vertices");
            return {(low + high) * 0.5f, (high - low) * 0.5f};
        }

        struct ParticleVolume
        {
            Entity Owner;
            Box* Bounds;
            Vector3 PreviousCenter;
            Matrix PreviousWorld;
            bool Initialized = false;
        };
        std::vector<std::unique_ptr<Box>> _boxes;
        std::vector<std::pair<Entity, Box*>> _tracked;
        std::vector<ParticleVolume> _particleVolumes;
    };
}
