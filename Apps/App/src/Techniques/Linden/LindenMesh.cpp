#include "LindenMesh.h"
#include "App.Base/Modules/SceneManagerModule.h"
#include "Engine.Core/Logging/Logger.h"
#include "Engine.Core/Types/MeshTypes.h"
#include "Engine.RendererDX12/GDX12Material.h"
#include <algorithm>
#include <map>
#include <numeric>
#include <string_view>

namespace BistroRender
{
namespace
{
struct LindenComponent : ComponentTag
{
    std::shared_ptr<const LindenMesh> Mesh;
};

constexpr std::string_view NodeName = "Linde_Tree_Large_linde_tree_large";
constexpr std::array<std::string_view, 3> Materials = {
    "Foliage_Linde_Tree_Large_Trunk", "Foliage_Linde_Tree_Large_Orange_Leaves", "Foliage_Linde_Tree_Large_Green_Leaves"
};

void AttachLeaves(LindenMesh& mesh, UINT firstVertex, UINT firstIndex, const Vector3& trunkCenter)
{
    const UINT count = static_cast<UINT>(mesh.Vertices.size()) - firstVertex;
    std::vector<UINT> parents(count);
    std::iota(parents.begin(), parents.end(), 0u);
    const auto root = [&](UINT i)
    {
        while (i != parents[i])
        {
            parents[i] = parents[parents[i]];
            i = parents[i];
        }
        return i;
    };
    // Preserve UV and normal seams in the draw data, but move both sides of a seam together.
    std::map<std::array<float, 3>, UINT> positions;
    for (UINT i = 0; i < count; ++i)
    {
        const auto& p = mesh.Vertices[firstVertex + i].Position;
        const auto found = positions.emplace(std::array<float, 3>{p.x, p.y, p.z}, i);
        parents[i] = found.first->second;
    }
    for (size_t i = firstIndex; i < mesh.Indices.size(); i += 3)
    {
        const UINT a = root(mesh.Indices[i] - firstVertex);
        parents[root(mesh.Indices[i + 1] - firstVertex)] = a;
        parents[root(mesh.Indices[i + 2] - firstVertex)] = a;
    }

    std::vector<UINT> fragmentForRoot(count, UINT_MAX);
    const UINT firstFragment = static_cast<UINT>(mesh.Fragments.size());
    for (UINT i = 0; i < count; ++i)
    {
        auto& vertex = mesh.Vertices[firstVertex + i];
        auto& fragment = fragmentForRoot[root(i)];
        if (fragment == UINT_MAX)
        {
            fragment = static_cast<UINT>(mesh.Fragments.size());
            const UINT phase = (fragment + 1u) * 2654435761u;
            mesh.Fragments.push_back({vertex.Position, static_cast<float>(phase >> 8) * (XM_2PI / 16777216.f)});
        }
        vertex.Fragment = fragment;
        auto& pivot = mesh.Fragments[fragment].Pivot;
        // The OBJ has no pivots. Use the innermost vertex of each connected leaf card.
        if (Vector3::DistanceSquared(vertex.Position, trunkCenter) < Vector3::DistanceSquared(pivot, trunkCenter))
            pivot = vertex.Position;
    }
    std::vector<float> radii(mesh.Fragments.size() - firstFragment, 0.f);
    for (UINT i = firstVertex; i < mesh.Vertices.size(); ++i)
    {
        auto& v = mesh.Vertices[i];
        v.Weight = Vector3::Distance(v.Position, mesh.Fragments[v.Fragment].Pivot);
        auto& radius = radii[v.Fragment - firstFragment];
        radius = (std::max)(radius, v.Weight);
    }
    for (UINT i = firstVertex; i < mesh.Vertices.size(); ++i)
    {
        auto& v = mesh.Vertices[i];
        const float radius = radii[v.Fragment - firstFragment];
        if (radius > 0.f) v.Weight /= radius;
    }
}
}

void ExtractLinden(World& world, const Engine::Core::Mesh& source, const StaticMeshRenderComponent& component,
                   std::vector<UINT>& staticParts)
{
    std::array<std::vector<UINT>, Materials.size()> groups;
    std::vector<UINT> remaining;
    remaining.reserve(staticParts.size());
    for (UINT index : staticParts)
    {
        const auto& part = source.GetSubMesh(index);
        if (part.SourceNodeName == NodeName)
        {
            const auto& name = source.GetMaterial(part.MaterialIndex).Name;
            const auto material = std::find(Materials.begin(), Materials.end(), name);
            if (material != Materials.end())
            {
                groups[material - Materials.begin()].push_back(index);
                continue;
            }
        }
        remaining.push_back(index);
    }
    if (std::any_of(groups.begin(), groups.end(), [](const auto& group) { return group.empty(); })) return;

    auto mesh = std::make_shared<LindenMesh>();
    Vector3 trunkCenter;
    for (size_t group = 0; group < groups.size(); ++group)
    {
        const auto firstIndex = static_cast<UINT>(mesh->Indices.size());
        const auto firstVertex = static_cast<UINT>(mesh->Vertices.size());
        for (UINT index : groups[group])
        {
            const auto& part = source.GetSubMesh(index);
            const auto base = static_cast<UINT>(mesh->Vertices.size());
            for (const auto& v : part.Vertices)
                mesh->Vertices.push_back({v.Position, UINT_MAX, v.Normal, 0.f, v.TexCoord});
            for (UINT i : part.Indices) mesh->Indices.push_back(base + i);
            if (!base) mesh->Bounds = part.Bounds;
            else DirectX::BoundingBox::CreateMerged(mesh->Bounds, mesh->Bounds, part.Bounds);
            if (group == 1 && base == firstVertex) mesh->CrownBounds = part.Bounds;
            else if (group) DirectX::BoundingBox::CreateMerged(mesh->CrownBounds, mesh->CrownBounds, part.Bounds);
        }
        if (group == 0)
        {
            mesh->WoodBounds = mesh->Bounds;
            trunkCenter = Vector3(mesh->WoodBounds.Center);
        }
        else AttachLeaves(*mesh, firstVertex, firstIndex, trunkCenter);

        const auto slot = source.GetSubMesh(groups[group].front()).MaterialIndex;
        mesh->Batches.push_back({firstIndex, static_cast<UINT>(mesh->Indices.size()) - firstIndex,
                                 component.Materials.at(slot)->_CBufferIndex, group != 0});
    }
    auto& ecs = world.GetECS();
    ecs.RegisterComponent<LindenComponent>();
    auto entity = ecs.CreateEntity();
    entity.AddComponent<NameComponent>(std::string(NodeName));
    entity.AddComponent<LindenComponent>().Mesh = mesh;
    staticParts = std::move(remaining);
    Log::Info("BistroLinden", "vertices=", mesh->Vertices.size(), "; triangles=", mesh->Indices.size() / 3,
              "; foliage fragments=", mesh->Fragments.size(), "; material batches=", mesh->Batches.size());
}

std::shared_ptr<const LindenMesh> FindLindenMesh(SceneManagerModule& scenes)
{
    for (size_t i = 0; i < scenes.GetWorldCount(); ++i)
    {
        auto& ecs = scenes.GetWorld(i)->GetECS();
        ecs.RegisterComponent<LindenComponent>();
        const auto& trees = ecs.GetPool<LindenComponent>().GetDense();
        if (!trees.empty()) return trees.front().Mesh;
    }
    return {};
}
}
