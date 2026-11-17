#pragma once

#include "App.Base/ECS/World.h"
#include "App.Base/Modules/RenderModule.h"
#include "Engine.Core/AssetManager.h"
#include "Engine.Core/Logging/Logger.h"
#include <algorithm>
#include <array>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace BistroPaintingSurfaces
{
    inline constexpr std::array<const char*, 3> Nodes = {
        "Paris_Painting_B", "Paris_Painting_F", "Paris_Painting_C"};
    inline constexpr std::array<const char*, 3> Names = {
        "BistroUICanvasNeon", "BistroUICanvasAstra", "BistroUICanvasClouds"};

    inline void Install(World& world, RenderModule& render, const StaticMeshRenderComponent& source,
        const std::array<GDX12Material*, 3>& materials, std::array<Entity, 3>& entities)
    {
        using namespace Engine::Core;
        auto& registry = AssetManager::GetInstance().Meshes();
        const auto* interior = registry.GetMesh(source.MeshHandler);
        if (!interior) throw std::runtime_error("Bistro UI requires the loaded interior mesh");

        std::vector<std::uint32_t> retained;
        std::array<const SubMesh*, 3> canvases{};
        const auto& parts = interior->GetSubMeshes();
        for (size_t local = 0; local < source.GetSubMeshCount(parts.size()); ++local)
        {
            const auto index = source.GetSubMeshIndex(local);
            const auto& part = parts.at(index);
            const auto found = std::find(Nodes.begin(), Nodes.end(), part.SourceNodeName);
            if (found == Nodes.end() || !materials[size_t(found - Nodes.begin())] ||
                interior->GetMaterial(part.MaterialIndex).Name != "Paris_Paintings")
            {
                retained.push_back(index);
                continue;
            }
            const auto slot = size_t(found - Nodes.begin());
            if (canvases[slot]) throw std::runtime_error(std::string("Duplicate Bistro painting canvas: ") + Nodes[slot]);
            canvases[slot] = &part;
        }
        if (retained.empty()) throw std::runtime_error("Bistro UI replacement must retain the original interior and frames");

        std::array<MeshHandle, 3> surfaces;
        for (size_t slot = 0; slot < Nodes.size(); ++slot)
        {
            if (!materials[slot]) { entities[slot] = InvalidEntity; continue; }
            const auto* canvas = canvases[slot];
            if (!canvas)
                throw std::runtime_error(std::string("Missing Bistro painting canvas/material: ") + Nodes[slot]);
            if (canvas->Indices.size() != 6 || canvas->Vertices.empty())
                throw std::runtime_error(std::string("Expected a two-triangle Bistro painting canvas: ") + Nodes[slot]);
            const auto name = std::string("BistroUI-") + Nodes[slot] + "-" +
                std::to_string(source.MeshHandler.GetValue()) + "-" + std::to_string(source.MeshHandler.GetGeneration()) + ".generated";
            const auto handle = registry.Register(MeshAssetLocator{std::filesystem::path(ASSETS_FOLDER) / "Models" / name});
            auto* mesh = registry.GetMesh(handle);
            if (!mesh)
            {
                auto surface = *canvas;
                Vector2 low{(std::numeric_limits<float>::max)(), (std::numeric_limits<float>::max)()};
                Vector2 high{-(std::numeric_limits<float>::max)(), -(std::numeric_limits<float>::max)()};
                for (const auto& vertex : surface.Vertices)
                {
                    low.x = (std::min)(low.x, vertex.TexCoord.x); low.y = (std::min)(low.y, vertex.TexCoord.y);
                    high.x = (std::max)(high.x, vertex.TexCoord.x); high.y = (std::max)(high.y, vertex.TexCoord.y);
                }
                const auto range = high - low;
                if (!(range.x > 0 && range.y > 0))
                    throw std::runtime_error(std::string("Bistro painting canvas has degenerate UVs: ") + Nodes[slot]);
                for (auto& vertex : surface.Vertices)
                    vertex.TexCoord = {(vertex.TexCoord.x - low.x) / range.x, (vertex.TexCoord.y - low.y) / range.y};
                surface.MaterialIndex = 0;
                const auto bounds = surface.Bounds;
                std::vector<SubMesh> generated;
                generated.push_back(std::move(surface));
                mesh = registry.Cache(handle, std::make_unique<Mesh>(std::move(generated), bounds,
                    std::vector<MeshMaterial>{MeshMaterial{}}));
            }
            render.SubmitMesh(mesh, handle);
            surfaces[slot] = handle;
            const auto& center = canvas->Bounds.Center;
            Log::Info("BistroUI", "Painting canvas ", Nodes[slot], " center=", center.x, ",", center.y, ",", center.z);
        }

        render.ReserveTransformCapacity(4);
        render.ReserveInstanceCapacity(UINT(retained.size() + surfaces.size()));
        auto& ecs = world.GetECS();
        std::vector<Entity> created;
        try
        {
            auto body = ecs.CreateEntity();
            created.push_back(body.GetId());
            body.AddComponent<NameComponent>("Interior_interior");
            body.AddComponent<TransformComponent>();
            auto& original = body.AddComponent<StaticMeshRenderComponent>(source.MeshHandler, source.Materials, std::move(retained));
            original.Visible = source.Visible;
            original.CastsShadows = source.CastsShadows;
            original.ReflectionDynamic = source.ReflectionDynamic;
            for (size_t slot = 0; slot < surfaces.size(); ++slot)
            {
                if (!materials[slot]) continue;
                auto entity = ecs.CreateEntity();
                created.push_back(entity.GetId());
                entity.AddComponent<NameComponent>(Names[slot]);
                entity.AddComponent<TransformComponent>();
                auto& draw = entity.AddComponent<StaticMeshRenderComponent>(surfaces[slot],
                    std::vector<GDX12Material*>{materials[slot]});
                draw.Visible = source.Visible;
                draw.CastsShadows = source.CastsShadows;
                draw.ReflectionDynamic = true;
                entities[slot] = entity.GetId();
            }
        }
        catch (...)
        {
            for (const auto entity : created) ecs.DestroyEntity(entity);
            entities.fill(InvalidEntity);
            throw;
        }
    }
}
