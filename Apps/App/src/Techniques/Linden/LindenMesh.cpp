#include "LindenMesh.h"
#include "App.Base/Modules/SceneManagerModule.h"
#include "Engine.Core/Logging/Logger.h"
#include "Engine.Core/Types/MeshTypes.h"
#include "Engine.RendererDX12/GDX12Material.h"
#include "App.Base/Modules/RenderModule.h"
#include "Engine.Core/AssetManager.h"
#include "Common/ConsoleVariables.h"
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
    std::array<std::shared_ptr<const LindenMesh>, 2> Lods;
};

constexpr std::string_view NodeName = "Linde_Tree_Large_linde_tree_large";
enum class TreeQuality : uint8_t { Low, High, Count };
AutoConsoleVariable<TreeQuality> Quality(L"Render.Tree.Quality", TreeQuality::High,
    L"Bistro linden simulation and draw geometry: 0 Low (LOD1), 1 High (LOD0)");

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
        // The mesh has no leaf pivots. Use the innermost vertex of each connected leaf card.
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

// The canopy can be asymmetric. Anchor the replacement at the foot of the trunk.
Vector3 TrunkFoot(const Engine::Core::SubMesh& wood)
{
    const float floor = wood.Bounds.Center.y - wood.Bounds.Extents.y;
    const float band = wood.Bounds.Extents.y * 0.02f;
    Vector3 lo(FLT_MAX), hi(-FLT_MAX);
    for (const auto& v : wood.Vertices)
        if (v.Position.y <= floor + band)
        {
            lo = Vector3::Min(lo, v.Position);
            hi = Vector3::Max(hi, v.Position);
        }
    return {(lo.x + hi.x) * .5f, floor, (lo.z + hi.z) * .5f};
}

UINT TreeMaterial(RenderModule& render, const std::filesystem::path& folder, bool leaves)
{
    const std::string prefix = leaves ? "Branch_Linden" : "Bark_Linden";
    if (auto* existing = render.GetMaterialByName(prefix)) return existing->_CBufferIndex;
    const auto texture = [&](const std::string& suffix, Engine::Core::ETextureType type)
    {
        const auto name = prefix + suffix;
        const auto* image = Engine::Core::AssetManager::GetInstance().LoadTexture(folder / (name + ".png"));
        if (!image) throw std::runtime_error("Missing linden texture: " + name);
        auto* gpu = render.CreateTexture(name, image, type);
        if (!gpu) throw std::runtime_error("Failed to upload linden texture: " + name);
        return gpu;
    };
    auto* material = render.CreateMaterial(prefix);
    material->Diffuse = texture("_Col", Engine::Core::ETextureType::Color);
    material->Normal = texture("_N", Engine::Core::ETextureType::Data);
    material->HasNormalMap = true;
    material->Type = leaves ? MaterialType::Masked : MaterialType::Opaque;
    return material->_CBufferIndex;
}

std::shared_ptr<LindenMesh> BuildLod(const Engine::Core::Mesh& source, const Vector3& offset,
                                    float scale, const std::array<UINT, 2>& materials, UINT lod)
{
    auto mesh = std::make_shared<LindenMesh>();
    mesh->Lod = lod;
    for (UINT group = 0; group < materials.size(); ++group)
    {
        const bool leaves = group == 1;
        const UINT firstIndex = UINT(mesh->Indices.size()), firstVertex = UINT(mesh->Vertices.size());
        for (const auto& part : source.GetSubMeshes())
        {
            if (source.GetMaterial(part.MaterialIndex).Name != (leaves ? "M_Branch.005" : "M_Bark.005")) continue;
            const UINT base = UINT(mesh->Vertices.size());
            for (const auto& v : part.Vertices)
                mesh->Vertices.push_back({v.Position * scale + offset, UINT_MAX, v.Normal, 0.f, v.TexCoord});
            for (UINT i : part.Indices) mesh->Indices.push_back(base + i);
        }
        if (firstVertex == mesh->Vertices.size()) throw std::runtime_error("Missing linden bark or foliage submesh");
        auto& bounds = leaves ? mesh->CrownBounds : mesh->WoodBounds;
        DirectX::BoundingBox::CreateFromPoints(bounds, mesh->Vertices.size() - firstVertex,
            &mesh->Vertices[firstVertex].Position, sizeof(LindenVertex));
        if (leaves) AttachLeaves(*mesh, firstVertex, firstIndex, Vector3(mesh->WoodBounds.Center));
        mesh->Batches.push_back({firstIndex, UINT(mesh->Indices.size()) - firstIndex, materials[group], leaves});
    }
    DirectX::BoundingBox::CreateMerged(mesh->Bounds, mesh->WoodBounds, mesh->CrownBounds);
    Log::Info("BistroLinden", "Georgeous LOD", lod, "; vertices=", mesh->Vertices.size(),
              "; triangles=", mesh->Indices.size()/3, "; foliage fragments=", mesh->Fragments.size());
    return mesh;
}
}

void ExtractLinden(World& world, const Engine::Core::Mesh& exterior, RenderModule& render, std::vector<UINT>& staticParts)
{
    const Engine::Core::SubMesh* wood = nullptr;
    DirectX::BoundingBox oldBounds;
    bool found = false;
    for (UINT i : staticParts)
    {
        const auto& part = exterior.GetSubMesh(i);
        if (part.SourceNodeName != NodeName) continue;
        if (!found) oldBounds = part.Bounds;
        else DirectX::BoundingBox::CreateMerged(oldBounds, oldBounds, part.Bounds);
        found = true;
        // Bistro splits the bark into the trunk and many small branches.
        if (exterior.GetMaterial(part.MaterialIndex).Name == "Foliage_Linde_Tree_Large_Trunk" &&
            (!wood || part.Bounds.Center.y - part.Bounds.Extents.y < wood->Bounds.Center.y - wood->Bounds.Extents.y))
            wood = &part;
    }
    if (!wood) return;

    const auto folder = std::filesystem::path(ASSETS_FOLDER) / "Models/GeorgeousLinden";
    LindenComponent tree;
    const std::array<UINT, 2> materials{TreeMaterial(render, folder, false), TreeMaterial(render, folder, true)};
    render.WaitForUploads();
    Vector3 offset;
    float scale = 1.f;
    for (UINT lod = 0; lod < tree.Lods.size(); ++lod)
    {
        const auto* model = Engine::Core::AssetManager::GetInstance().LoadMesh(folder / (lod ? "SM_Linden_.fbx" : "SM_Linden.fbx"));
        if (!model || model->IsEmpty()) throw std::runtime_error("Cannot load Georgeous linden LOD");
        if (!lod)
        {
            const auto& parts = model->GetSubMeshes();
            const auto trunk = std::find_if(parts.begin(), parts.end(), [&](const auto& p)
                { return model->GetMaterial(p.MaterialIndex).Name == "M_Bark.005"; });
            if (trunk == parts.end() || model->GetBounds().Extents.y <= 0)
                throw std::runtime_error("Invalid high detail linden");
            scale = oldBounds.Extents.y / model->GetBounds().Extents.y;
            offset = TrunkFoot(*wood) - TrunkFoot(*trunk) * scale;
        }
        tree.Lods[lod] = BuildLod(*model, offset, scale, materials, lod);
    }
    auto& ecs = world.GetECS();
    ecs.RegisterComponent<LindenComponent>();
    auto entity = ecs.CreateEntity();
    entity.AddComponent<NameComponent>(std::string(NodeName));
    entity.AddComponent<LindenComponent>() = std::move(tree);
    staticParts.erase(std::remove_if(staticParts.begin(), staticParts.end(), [&](UINT i)
        { return exterior.GetSubMesh(i).SourceNodeName == NodeName; }), staticParts.end());
}

std::shared_ptr<const LindenMesh> FindLindenMesh(SceneManagerModule& scenes)
{
    for (size_t i = 0; i < scenes.GetWorldCount(); ++i)
    {
        auto& ecs = scenes.GetWorld(i)->GetECS();
        ecs.RegisterComponent<LindenComponent>();
        const auto& trees = ecs.GetPool<LindenComponent>().GetDense();
        if (!trees.empty()) return trees.front().Lods[Quality.GetValue() == TreeQuality::Low ? 1 : 0];
    }
    return {};
}
}
