#pragma once

#include "App.Base/ECS/World.h"
#include "App.Base/Modules/RenderModule.h"
#include "Engine.Core/AssetManager.h"
#include "Engine.UI/UI_test.h"
#include "Common/ConsoleVariables.h"
#include <algorithm>
#include <cfloat>
#include <memory>
#include <vector>

namespace BistroBounds
{
    inline AutoConsoleVariable<bool> Show(L"App.Bounds.Show", true,
        L"Show detached effect layout boxes; edits never change real scene objects. Session only.");

    struct Box
    {
        Box(const std::string& name, const BoundingBox& bounds, unsigned color, float yawDegrees = 0)
            : Name(name), Color(color),
              CenterX(Prefix() + L"CenterX", bounds.Center.x, L"Detached box centre X, world cm"),
              CenterY(Prefix() + L"CenterY", bounds.Center.y, L"Detached box centre Y, world cm"),
              CenterZ(Prefix() + L"CenterZ", bounds.Center.z, L"Detached box centre Z, world cm"),
              HalfExtentX(Prefix() + L"HalfExtentX", (std::max)(0.01f, bounds.Extents.x), L"Detached box half-size X, cm", EConsoleVariableFlags::Default, Positive),
              HalfExtentY(Prefix() + L"HalfExtentY", (std::max)(0.01f, bounds.Extents.y), L"Detached box half-size Y, cm", EConsoleVariableFlags::Default, Positive),
              HalfExtentZ(Prefix() + L"HalfExtentZ", (std::max)(0.01f, bounds.Extents.z), L"Detached box half-size Z, cm", EConsoleVariableFlags::Default, Positive),
              Visible(Prefix() + L"Visible", true, L"Show this detached layout box"), _yawDegrees(yawDegrees) {}

        BoundingBox GetBounds() const
        {
            return {{CenterX.GetValue(), CenterY.GetValue(), CenterZ.GetValue()},
                {HalfExtentX.GetValue(), HalfExtentY.GetValue(), HalfExtentZ.GetValue()}};
        }

        void Draw(const XMFLOAT4X4& viewProjection) const
        {
            const auto bounds = GetBounds();
            if (_yawDegrees == 0)
            {
                Engine::UI::DrawWorldBox(bounds, viewProjection, Name.c_str(), Color);
                return;
            }
            const auto world = Matrix::CreateRotationY(XMConvertToRadians(_yawDegrees)) *
                Matrix::CreateTranslation(Vector3(bounds.Center));
            XMFLOAT4X4 localViewProjection;
            XMStoreFloat4x4(&localViewProjection, world * XMLoadFloat4x4(&viewProjection));
            Engine::UI::DrawWorldBox({Vector3::Zero, bounds.Extents}, localViewProjection, Name.c_str(), Color);
        }

        const std::string Name;
        const unsigned Color;
        AutoConsoleVariable<float> CenterX, CenterY, CenterZ, HalfExtentX, HalfExtentY, HalfExtentZ;
        AutoConsoleVariable<bool> Visible;

    private:
        float _yawDegrees;
        std::wstring Prefix() const { return L"App.Bounds." + std::wstring(Name.begin(), Name.end()) + L"."; }
        static bool Positive(float value) { return value >= 0.01f && value <= 1000000; }
    };

    class Scene
    {
    public:
        // Copy once: later object motion, count changes and box edits remain independent.
        explicit Scene(World& world)
        {
            auto& ecs = world.GetECS();
            const auto add = [&](Entity entity, const BoundingBox& local, unsigned color)
            {
                BoundingBox bounds;
                local.Transform(bounds, WorldMatrix(ecs.Get<TransformComponent>(entity)));
                _boxes.push_back(std::make_unique<Box>(ecs.Get<NameComponent>(entity).value, bounds, color));
            };
            ecs.ForEach<ParticleEmitterComponent, TransformComponent, NameComponent>(
                [&](Entity entity, const ParticleEmitterComponent& emitter, const TransformComponent&, const NameComponent& name)
                {
                    if (name.value == "BistroInteriorDust")
                    {
                        _boxes.push_back(std::make_unique<Box>(name.value,
                            BoundingBox({559,221,-348}, {694,181,779}), 0xff40e8ff));
                        return;
                    }
                    if (name.value == "BistroTableSteam")
                    {
                        _boxes.push_back(std::make_unique<Box>(name.value,
                            BoundingBox({922,227,-293}, {305,175,85.5f}), 0xff40e8ff, -28.5f));
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
                [&](Entity, const SkeletalMeshRenderComponent& mesh, const TransformComponent& transform, const NameComponent& name)
                { _boxes.push_back(std::make_unique<Box>(name.value, CharacterBounds(mesh, transform), 0xffff70ff)); });
        }

        void DrawBounds(RenderModule& render) const
        {
            if (!Show.GetValue()) return;
            const auto* common = render.GetRenderPipelineCommonData();
            XMFLOAT4X4 viewProjection;
            XMStoreFloat4x4(&viewProjection, common->CameraView * common->CameraProjection);
            for (const auto& box : _boxes)
                if (box->Visible.GetValue())
                    box->Draw(viewProjection);
        }

    private:
        static Matrix WorldMatrix(const TransformComponent& transform)
        {
            return Matrix::CreateScale(transform.Scale) * Matrix::CreateFromYawPitchRoll(
                XMConvertToRadians(transform.Rotation.y), XMConvertToRadians(transform.Rotation.x),
                XMConvertToRadians(transform.Rotation.z)) * Matrix::CreateTranslation(transform.Location);
        }

        static BoundingBox CharacterBounds(const SkeletalMeshRenderComponent& component, const TransformComponent& transform)
        {
            auto& assets = Engine::Core::AssetManager::GetInstance();
            const auto* scene = assets.Scenes().GetScene(component.GetScene());
            if (!scene) throw std::runtime_error("Cannot snapshot bounds of an unloaded character");
            const auto pose = scene->EvaluatePose(component.GetClip(), component.GetTimeSeconds(),
                component.IsLooping(), component.GetLocalPose());
            const auto& nodes = scene->GetNodes();
            const auto world = WorldMatrix(transform);
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
                            position = Vector3::Transform(position, world);
                            low = Vector3::Min(low, position);
                            high = Vector3::Max(high, position);
                            found = true;
                        }
                    }
                }
            if (!found) throw std::runtime_error("Character bounds have no vertices");
            return {(low + high) * 0.5f, (high - low) * 0.5f};
        }

        std::vector<std::unique_ptr<Box>> _boxes;
    };
}
