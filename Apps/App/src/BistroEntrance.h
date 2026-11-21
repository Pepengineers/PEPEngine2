#pragma once

#include "App.Base/ECS/World.h"
#include "App.Base/Modules/RenderModule.h"
#include "Engine.Core/AssetManager.h"
#include "Engine.Core/BenchmarkEngine.h"

namespace BistroEntrance
{
inline const Vector3 Center{-80.037f, 166.15f, -94.836f};
inline constexpr float Yaw = 28.5f;
inline constexpr float HalfWidth = 89.39f;
inline constexpr std::array Names{"BistroEntranceDoorLeft", "BistroEntranceDoorRight"};

inline Matrix Rotation(float yaw)
{
    return Matrix::CreateRotationY(XMConvertToRadians(yaw));
}

inline float Side(size_t leaf)
{
    return leaf == 0 ? 1.f : -1.f;
}

inline Vector3 Hinge(size_t leaf)
{
    return Vector3(Center.x, 0, Center.z) +
        Vector3::Transform({0, 0, Side(leaf) * HalfWidth}, Rotation(Yaw));
}

inline void Install(World& world, const StaticMeshRenderComponent& source,
    const Engine::Core::Mesh& exterior, const std::vector<uint32_t>& entrance)
{
    using namespace Engine::Core;
    auto& render = *BenchmarkEngine::GetLocator().GetModule<RenderModule>();
    auto& registry = AssetManager::GetInstance().Meshes();
    render.ReserveTransformCapacity(static_cast<UINT>(Names.size()));
    render.ReserveInstanceCapacity(static_cast<UINT>(entrance.size()));
    const auto across = Vector3::Transform(Vector3(0, 0, 1), Rotation(Yaw));
    for (size_t leaf = 0; leaf < Names.size(); ++leaf)
    {
        const auto path = std::filesystem::path(ASSETS_FOLDER) / "Models" /
            (std::string(Names[leaf]) + "-" + std::to_string(source.MeshHandler.GetValue()) + "-" +
                std::to_string(source.MeshHandler.GetGeneration()) + ".generated");
        const auto handle = registry.Register(MeshAssetLocator{path});
        auto* mesh = registry.GetMesh(handle);
        if (!mesh)
        {
            std::vector<SubMesh> parts;
            DirectX::BoundingBox bounds;
            for (const auto index : entrance)
            {
                const auto& input = exterior.GetSubMesh(index);
                SubMesh part;
                part.MaterialIndex = input.MaterialIndex;
                part.SourceNodeName = input.SourceNodeName;
                for (size_t i = 0; i + 2 < input.Indices.size(); i += 3)
                {
                    const auto& a = input.Vertices.at(input.Indices[i]);
                    const auto& b = input.Vertices.at(input.Indices[i + 1]);
                    const auto& c = input.Vertices.at(input.Indices[i + 2]);
                    // The authored leaves overlap slightly at the seam.
                    const bool positive = ((a.Position + b.Position + c.Position) / 3 - Center).Dot(across) >= 0;
                    if (positive != (leaf == 0)) continue;
                    for (const auto* vertex : {&a, &b, &c})
                    {
                        part.Indices.push_back(static_cast<uint32_t>(part.Vertices.size()));
                        part.Vertices.push_back(*vertex);
                    }
                }
                if (part.Indices.empty()) continue;
                DirectX::BoundingBox::CreateFromPoints(part.Bounds, part.Vertices.size(),
                    &part.Vertices.front().Position, sizeof(Vertex));
                if (parts.empty()) bounds = part.Bounds;
                else DirectX::BoundingBox::CreateMerged(bounds, bounds, part.Bounds);
                parts.push_back(std::move(part));
            }
            if (parts.empty()) throw std::runtime_error("Missing Bistro entrance door leaf");
            mesh = registry.Cache(handle, std::make_unique<Mesh>(std::move(parts), bounds, exterior.GetMaterials()));
        }
        render.SubmitMesh(mesh, handle);
        auto door = world.GetECS().CreateEntity();
        door.AddComponent<NameComponent>(Names[leaf]);
        door.AddComponent<TransformComponent>(Vector3::Zero);
        auto& draw = door.AddComponent<StaticMeshRenderComponent>(handle, source.Materials);
        draw.Visible = source.Visible;
        draw.CastsShadows = source.CastsShadows;
        draw.ReflectionDynamic = true;
    }
}
}
