#pragma once

#include "Engine.Core/Types/MeshTypes.h"
#include "App.Base/ECS/World.h"
#include "App.Base/Modules/PhysicsModule.h"
#include "App.Base/Modules/RenderModule.h"
#include "Common/ConsoleVariables.h"
#include "BistroWaves.h"
#include "BistroStaticCollision.h"
#include "App.Base/Physics/CollisionGeometry.h"
#include "Engine.Core/Jobs/JobSystem.h"
#include "App.Base/App.h"
#include <cmath>
#include <stdexcept>
#include <algorithm>
#include <cctype>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <array>
#include "Engine.Core/Logging/Logger.h"

enum class BistroPropCategory : unsigned { Unknown, Glass, Plates, Cutlery, Bottles, Furniture, Fabric, Baskets, OtherMovable, Count };
inline const char* BistroPropCategoryName(BistroPropCategory category)
{
    constexpr const char* names[] = {"Unknown", "Glass", "Plates", "Cutlery", "Bottles", "Furniture", "Fabric", "Baskets", "OtherMovable"};
    return names[static_cast<unsigned>(category)];
}
inline BistroPropCategory ClassifyBistroProp(std::string lower)
{
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const auto has = [&](const char* token) { return lower.find(token) != std::string::npos; };
    const auto starts = [&](const char* token) { return lower.rfind(token, 0) == 0; };
    if (has("glass_water") || has("glass_wine") || has("glass_beer") || has("glass_detailed")) return BistroPropCategory::Glass;
    if (starts("paris_plate") || starts("plates_") || starts("trolley_paris_plate")) return BistroPropCategory::Plates;
    if (starts("cutlery_") || starts("trolley_paris_cutlery")) return BistroPropCategory::Cutlery;
    if (has("liquorbottle")) return BistroPropCategory::Bottles;
    if (starts("paris_table_") || starts("paris_chair_") || starts("paris_barstool_") || starts("paris_coat_rack") ||
        starts("paris_wine_cooler") || starts("cooler_02_paris_wine_cooler") || starts("paris_bar_cart")) return BistroPropCategory::Furniture;
    if (starts("cotton_placemat") || starts("cooler_cloth") || starts("doormat") || starts("paris_rubber_bar_mat")) return BistroPropCategory::Fabric;
    if (starts("wickerbasket")) return BistroPropCategory::Baskets;
    if (has("napkinholder") || starts("coasters_") || starts("paris_toffeejar") || starts("cookiejar") ||
        starts("paris_flower_pot") || starts("cashregister")) return BistroPropCategory::OtherMovable;
    return BistroPropCategory::Unknown;
}
inline bool BistroPropDynamic(BistroPropCategory category, int quality)
{
    return (quality >= 1 && category == BistroPropCategory::Glass) ||
        (quality >= 2 && category == BistroPropCategory::Plates) ||
        (quality >= 3 && category == BistroPropCategory::Cutlery) ||
        (quality >= 4 && category != BistroPropCategory::Unknown);
}

struct BistroPropGroup
{
    std::string Name;
    std::vector<std::uint32_t> SubMeshIndices;
    BistroPropCategory Category = BistroPropCategory::Unknown;
};

struct BistroPropPartition
{
    std::vector<std::uint32_t> StaticSubMeshes;
    std::vector<BistroPropGroup> Props;
    std::vector<size_t> Tableware;
    bool Interior = true;
    std::vector<std::string> StaticObjects;
};

inline bool IsBistroTableware(const std::string& lower)
{
    for (const auto* family : {"liquorbottle", "glass_water", "glass_wine", "glass_beer", "glass_detailed",
        "plate", "cutlery", "napkinholder", "placemat", "wickerbasket", "coaster"})
        if (lower.find(family) != std::string::npos) return true;
    return false;
}

inline BistroPropPartition PartitionBistroProps(const Engine::Core::Mesh& mesh, bool interior = true, bool categorized = false)
{
    BistroPropPartition result;
    result.Interior = interior;
    std::unordered_map<std::string, size_t> groups;
    const auto& parts = mesh.GetSubMeshes();
    std::unordered_set<std::string> staticObjects;
    for (size_t index = 0; index < parts.size(); ++index)
    {
        const auto& name = parts[index].SourceNodeName;
        std::string lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const auto category = ClassifyBistroProp(lower);
        if (!interior || (categorized ? category == BistroPropCategory::Unknown :
            (!IsBistroTableware(lower) && lower.find("chair") == std::string::npos && lower.find("table") == std::string::npos)))
        {
            result.StaticSubMeshes.push_back(static_cast<std::uint32_t>(index));
            if (staticObjects.insert(name).second) result.StaticObjects.push_back(name);
            continue;
        }
        auto objectName = name;
        if (categorized && objectName.rfind("CookieJar_Glass", 0) == 0) objectName.erase(9, 6);
        const auto section = objectName.find("_Mesh_");
        if (section != std::string::npos && section + 8 <= objectName.size()) objectName.erase(section, 8);
        const auto inserted = groups.emplace(objectName, result.Props.size());
        if (inserted.second)
        {
            if (IsBistroTableware(lower)) result.Tableware.push_back(result.Props.size());
            result.Props.push_back({ objectName, {}, category });
        }
        result.Props[inserted.first->second].SubMeshIndices.push_back(static_cast<std::uint32_t>(index));
    }
    return result;
}

namespace BistroPhysics
{
    inline float CrossXZ(const Vector3& a, const Vector3& b, const Vector3& p)
    { return (b.x-a.x)*(p.z-a.z)-(b.z-a.z)*(p.x-a.x); }

    inline std::vector<Vector3> SpawnFootprint(std::vector<Vector3> points)
    {
        std::sort(points.begin(),points.end(),[](const auto& a,const auto& b){ return a.x<b.x || (a.x==b.x && a.z<b.z); });
        points.erase(std::unique(points.begin(),points.end(),[](const auto& a,const auto& b){ return a.x==b.x && a.z==b.z; }),points.end());
        if (points.size()<3) throw std::invalid_argument("Indoor spawn footprint needs three distinct points");
        std::vector<Vector3> hull;
        for (const auto& point : points)
        {
            while (hull.size()>1 && CrossXZ(hull[hull.size()-2],hull.back(),point)<=0) hull.pop_back();
            hull.push_back(point);
        }
        const auto lower=hull.size();
        for (size_t i=points.size()-1; i-->0;)
        {
            while (hull.size()>lower && CrossXZ(hull[hull.size()-2],hull.back(),points[i])<=0) hull.pop_back();
            hull.push_back(points[i]);
        }
        hull.pop_back();
        if (hull.size()<3) throw std::invalid_argument("Degenerate indoor spawn footprint");
        return hull;
    }

    inline bool FitsFootprint(const std::vector<Vector3>& hull, const Vector3& center, const Vector3& half)
    {
        for (size_t i=0; i<hull.size(); ++i)
            for (float x : {-half.x,half.x}) for (float z : {-half.z,half.z})
                if (CrossXZ(hull[i],hull[(i+1)%hull.size()],center+Vector3(x,0,z))<0) return false;
        return hull.size()>=3;
    }

    inline constexpr uint32_t MaxBodies = 16384;
    inline AutoConsoleVariable<bool> Categorized(L"App.Physics.Categorized", false,
        L"Opt in to authored interior category physics; ignores CountScale/ActiveTablewareFraction. Reload world to apply.");
    inline AutoConsoleVariable<float> GlassOpacity(L"App.Physics.GlassOpacity", .85f,
        L"Opacity of categorized Bistro glassware, 0..1; reload Bistro to apply.",
        EConsoleVariableFlags::Default, [](float value) { return std::isfinite(value) && value >= 0 && value <= 1; });
    inline AutoConsoleVariable<int> Quality(L"App.Physics.Quality", 1,
        L"Interior categories: 0 Off, 1 glasses, 2 +plates, 3 +cutlery, 4 all movable props. Reload world to apply.",
        EConsoleVariableFlags::Default, [](int value) { return value >= 0 && value <= 4; });
    inline bool EffectiveCategorized = false;
    inline int EffectiveQuality = -1;
    inline AutoConsoleVariableRef<bool> EffectiveCategorizedDisplay(L"App.Physics.EffectiveCategorized", EffectiveCategorized,
        L"Category mode latched for the loaded world", EConsoleVariableFlags::ReadOnly);
    inline AutoConsoleVariableRef<int> EffectiveQualityDisplay(L"App.Physics.EffectiveQuality", EffectiveQuality,
        L"Category quality latched for the loaded world; -1 means legacy", EConsoleVariableFlags::ReadOnly);
    inline AutoConsoleVariable<double> CountScale(L"App.Physics.CountScale", 1.0,
        L"Indoor tableware count, 0..16: >1 multiplies bottles/tableware only; original furniture is independent; reload to apply",
        EConsoleVariableFlags::Default, [](double value) { return value >= 0 && value <= 16; });
    inline AutoConsoleVariable<double> ActiveTablewareFraction(L"App.Physics.ActiveTablewareFraction", 1.0,
        L"Fraction of authored tableware with dynamic bodies, 0..1. Restart Bistro to apply.",
        EConsoleVariableFlags::Default, [](double value) { return value >= 0 && value <= 1; });
    inline AutoConsoleVariable<bool> SimulationEnabled(L"App.Physics.SimulationEnabled", true,
        L"Simulate indoor props. When off, keep the authored meshes and static colliders. Restart Bistro to apply.");


    inline size_t GetCount(size_t tableware, double scale, size_t furniture = 0)
    {
        if (!std::isfinite(scale) || scale < 0 || scale > 16 || tableware > MaxBodies - 3 || furniture > MaxBodies - 3 - tableware)
            throw std::invalid_argument("Invalid Bistro physics spawn budget");
        const double count = std::ceil(static_cast<double>(tableware) * (std::max)(1.0, scale)) + furniture;
        if (count > MaxBodies - 3) throw std::length_error("Bistro physics body budget exceeded");
        return static_cast<size_t>(count);
    }

    inline size_t GetCount(const BistroPropPartition& partition, double scale)
    {
        return GetCount(partition.Tableware.size(), scale, partition.Props.size()-partition.Tableware.size());
    }

    inline std::vector<bool> SelectDynamicProps(const BistroPropPartition& partition,
        const std::vector<DirectX::BoundingBox>& bounds, double fraction, bool enabled)
    {
        if (bounds.size() != partition.Props.size() || !std::isfinite(fraction) || fraction < 0 || fraction > 1)
            throw std::invalid_argument("Invalid Bistro dynamic prop selection");
        std::vector<bool> dynamic(bounds.size(), enabled);
        if (!enabled || fraction == 1) return dynamic;
        auto order = partition.Tableware;
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b)
        {
            const auto& left = bounds[a].Center;
            const auto& right = bounds[b].Center;
            if (left.x != right.x) return left.x < right.x;
            return left.z < right.z;
        });
        const auto active = static_cast<size_t>(std::ceil(fraction * order.size()));
        size_t picked = 0;
        for (size_t i = 0; i < order.size(); ++i)
        {
            const auto next = (i + 1) * active / order.size();
            dynamic[order[i]] = next != picked;
            picked = next;
        }
        return dynamic;
    }

    inline PhysicsWorldSettings Settings()
    {
        // Use a 1 mm contact tolerance for small tableware.
        return {MaxBodies, 131072, 131072, 100, 0.001f, 4};
    }

    inline DirectX::BoundingBox Bounds(const Engine::Core::Mesh& mesh, const std::vector<uint32_t>& indices)
    {
        if (indices.empty()) throw std::invalid_argument("Empty Bistro object");
        auto bounds = mesh.GetSubMesh(indices.front()).Bounds;
        for (size_t i = 1; i < indices.size(); ++i)
            DirectX::BoundingBox::CreateMerged(bounds, bounds, mesh.GetSubMesh(indices[i]).Bounds);
        return bounds;
    }

    inline PhysicsCompoundChild BoxProxy(std::vector<Vector3> points, const Quaternion* worldToBox = nullptr)
    {
        if (points.empty()) throw std::invalid_argument("Empty Bistro box proxy");
        const auto origin = points.front();
        for (auto& point : points)
        {
            point -= origin;
            if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z))
                throw std::invalid_argument("Nonfinite Bistro box proxy");
            if (worldToBox) point = Vector3::Transform(point, *worldToBox);
        }
        DirectX::BoundingOrientedBox box;
        if (worldToBox)
        {
            DirectX::BoundingBox aligned;
            DirectX::BoundingBox::CreateFromPoints(aligned, points.size(), points.data(), sizeof(Vector3));
            Quaternion inverse; worldToBox->Conjugate(inverse);
            box.Center = Vector3::Transform(Vector3(aligned.Center), inverse);
            box.Extents = aligned.Extents;
            box.Orientation = inverse;
        }
        else
        {
            DirectX::BoundingOrientedBox::CreateFromPoints(box, points.size(), points.data(), sizeof(Vector3));
            const auto& q = box.Orientation;
            const float length = q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w;
            if (!std::isfinite(box.Center.x) || !std::isfinite(box.Center.y) || !std::isfinite(box.Center.z) ||
                !std::isfinite(box.Extents.x) || !std::isfinite(box.Extents.y) || !std::isfinite(box.Extents.z) ||
                !std::isfinite(length) || length <= 1e-12f)
            {
                // PCA may be singular for symmetric/flat authored groups; their AABB is still a valid proxy.
                DirectX::BoundingBox aligned;
                DirectX::BoundingBox::CreateFromPoints(aligned, points.size(), points.data(), sizeof(Vector3));
                box.Center = aligned.Center;
                box.Extents = aligned.Extents;
                box.Orientation = Quaternion::Identity;
            }
        }
        // Thin furniture surfaces still need a solid collision thickness (centimeters).
        const Vector3 half((std::max)(box.Extents.x, 0.5f), (std::max)(box.Extents.y, 0.5f), (std::max)(box.Extents.z, 0.5f));
        return {BoxShapeDesc{half}, Vector3(box.Center) + origin, Quaternion(box.Orientation)};
    }

    inline PhysicsShapeDesc CategoryShape(const Engine::Core::Mesh& mesh, const BistroPropGroup& prop)
    {
        std::vector<Vector3> objectPoints;
        for (const auto index : prop.SubMeshIndices)
            for (const auto& vertex : mesh.GetSubMesh(index).Vertices) objectPoints.push_back(vertex.Position);
        const auto objectBox = BoxProxy(objectPoints);
        auto compound = std::make_shared<PhysicsCompound>();
        const auto add = [&](CompoundChildShapeDesc shape, Vector3 center, Quaternion rotation = Quaternion::Identity) {
            compound->Children.push_back({std::move(shape), center, rotation});
        };
        if (prop.Category == BistroPropCategory::Glass || prop.Category == BistroPropCategory::Bottles || prop.Category == BistroPropCategory::Plates)
        {
            const auto half = std::get<BoxShapeDesc>(objectBox.Shape).HalfExtent;
            const float axes[] = {half.x, half.y, half.z};
            unsigned axis = 0;
            for (unsigned i = 1; i < 3; ++i)
                if (prop.Category == BistroPropCategory::Plates ? axes[i] < axes[axis] : axes[i] > axes[axis]) axis = i;
            const float height = axes[axis];
            const float radius = (std::max)(axes[(axis+1)%3], axes[(axis+2)%3]);
            const auto align = axis == 0 ? Matrix::CreateRotationZ(-DirectX::XM_PIDIV2) :
                axis == 2 ? Matrix::CreateRotationX(DirectX::XM_PIDIV2) : Matrix::Identity;
            const auto rotation = Quaternion::CreateFromRotationMatrix(align * Matrix::CreateFromQuaternion(objectBox.Rotation));
            if (prop.Category == BistroPropCategory::Bottles && height > radius + .1f)
                add(CapsuleShapeDesc{radius, height-radius}, objectBox.Position, rotation);
            else add(CylinderShapeDesc{radius, height}, objectBox.Position, rotation);
        }
        else if (prop.Category == BistroPropCategory::Furniture)
        {
            const auto& points = objectPoints;
            Quaternion toLocal = Quaternion::Identity;
            float area = FLT_MAX;
            for (int i = 0; i < 90; ++i)
            {
                const auto rotation = Quaternion::CreateFromYawPitchRoll(DirectX::XMConvertToRadians(float(i)), 0, 0);
                Vector3 lo(FLT_MAX), hi(-FLT_MAX);
                for (const auto& point : points) { const auto p = Vector3::Transform(point, rotation); lo = Vector3::Min(lo, p); hi = Vector3::Max(hi, p); }
                const float next = (hi.x-lo.x)*(hi.z-lo.z);
                if (next < area) { area = next; toLocal = rotation; }
            }
            auto box = BoxProxy(points, &toLocal);
            const auto extents = std::get<BoxShapeDesc>(box.Shape).HalfExtent;
            const auto addBox = [&](Vector3 center, Vector3 size) {
                add(BoxShapeDesc{Vector3::Max(size, Vector3(.5f)), 0},
                    box.Position + Vector3::Transform(center, box.Rotation), box.Rotation);
            };
            std::string lower = prop.Name;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            const bool table = lower.rfind("paris_table_", 0) == 0;
            const bool stool = lower.rfind("paris_barstool_", 0) == 0;
            const bool chair = lower.rfind("paris_chair_", 0) == 0 || stool;
            if (table || chair)
            {
                const float seat = table ? extents.y - 3 : (std::min)(stool ? 75.f : 45.f, extents.y * 1.4f) - extents.y;
                addBox({0,seat,0}, {extents.x,3,extents.z});
                const float bottom = -extents.y;
                const float legHalf = (seat-3-bottom)*.5f;
                for (float x : {-1.f,1.f}) for (float z : {-1.f,1.f})
                    addBox({x*(extents.x-3),bottom+legHalf,z*(extents.z-3)}, {2,legHalf,2});
                if (chair)
                {
                    float backZ = 0;
                    size_t backPoints = 0;
                    for (const auto& point : points)
                    {
                        const auto local = Vector3::Transform(point - box.Position, toLocal);
                        if (local.y > seat + 6) { backZ += local.z; ++backPoints; }
                    }
                    if (backPoints) addBox({0,(seat+3+extents.y)*.5f,(backZ < 0 ? -1.f : 1.f)*(extents.z-2)},
                        {extents.x,(extents.y-seat-3)*.5f,2});
                }
            }
            else compound->Children.push_back(box);
        }
        else
        {
            compound->Children.push_back(objectBox);
        }
        return CompoundShapeDesc{compound};
    }

    inline PhysicsShapeDesc PropShape(const Engine::Core::Mesh& mesh, const BistroPropGroup& prop)
    {
        std::string lower = prop.Name;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (IsBistroTableware(lower))
        {
            std::vector<Vector3> points;
            for (const auto index : prop.SubMeshIndices)
                for (const auto& vertex : mesh.GetSubMesh(index).Vertices) points.push_back(vertex.Position);
            auto compound = std::make_shared<PhysicsCompound>();
            compound->Children.push_back(BoxProxy(std::move(points)));
            return CompoundShapeDesc{compound};
        }
        const Vector3 size = Vector3(Bounds(mesh, prop.SubMeshIndices).Extents) * 2;
        // Thin props cannot use the same detail budget as full-size furniture at 1 cm resolution.
        const auto hulls = static_cast<unsigned>(std::clamp(std::ceil((std::min)({size.x,size.y,size.z}) / 4.f), 4.f, 12.f));
        const auto baked = BuildDynamicCollision(mesh, prop.SubMeshIndices, 1.0f, hulls);
        std::vector<Vector3> points;
        for (const auto index : prop.SubMeshIndices)
            for (const auto& vertex : mesh.GetSubMesh(index).Vertices) points.push_back(vertex.Position);
        Quaternion frame = Quaternion::Identity;
        float bestArea = FLT_MAX;
        for (int step = 0; step < 180; ++step)
        {
            const auto rotation = Quaternion::CreateFromYawPitchRoll(DirectX::XMConvertToRadians(step * .5f), 0, 0);
            Vector3 lo = Vector3::Transform(points.front(), rotation), hi = lo;
            for (const auto& point : points)
            {
                const auto p = Vector3::Transform(point, rotation);
                lo = Vector3::Min(lo, p); hi = Vector3::Max(hi, p);
            }
            const float area = (hi.x-lo.x)*(hi.z-lo.z);
            if (area < bestArea) { bestArea = area; frame = rotation; }
        }
        auto compound = std::make_shared<PhysicsCompound>();
        if (const auto* convex = std::get_if<ConvexCompoundShapeDesc>(&baked))
        {
            for (const auto& hull : convex->Geometry->Hulls) compound->Children.push_back(BoxProxy(hull, &frame));
        }
        else if (const auto* mixed = std::get_if<CompoundShapeDesc>(&baked))
        {
            for (const auto& child : mixed->Geometry->Children)
                if (const auto* convex = std::get_if<ConvexCompoundShapeDesc>(&child.Shape))
                    for (const auto& hull : convex->Geometry->Hulls)
                    {
                        auto transformed = hull;
                        for (auto& point : transformed) point = Vector3::Transform(point, child.Rotation) + child.Position;
                        compound->Children.push_back(BoxProxy(std::move(transformed), &frame));
                    }
                else compound->Children.push_back(child);
        }
        else return baked;
        return CompoundShapeDesc{compound};
    }
}

inline void CreateBistroPropEntities(World& world, const std::string& sceneName,
    const StaticMeshRenderComponent& source, const Engine::Core::Mesh& mesh,
    const BistroPropPartition& partition, JobSystem& jobs, PhysicsModule* physics = nullptr, BistroWaves::Controller* waves = nullptr,
    bool categorized = false, int quality = 1,
    const std::function<bool(const StaticMeshRenderComponent&)>& architecture = {})
{
    auto& ecs = world.GetECS();
    const auto create = [&](const std::string& name, const std::vector<uint32_t>& indices,
        const PhysicsShapeDesc& shape, PhysicsMotionType motion, Vector3 offset = Vector3::Zero)
    {
        auto entity = ecs.CreateEntity();
        entity.AddComponent<NameComponent>(name);
        entity.AddComponent<TransformComponent>(offset);
        if (!indices.empty())
        {
            auto materials = source.Materials;
            if (categorized && ClassifyBistroProp(name) == BistroPropCategory::Glass)
            {
                auto& render = *BenchmarkEngine::GetLocator().GetModule<RenderModule>();
                for (const auto index : indices)
                {
                    const auto slot = mesh.GetSubMesh(index).MaterialIndex;
                    const auto* original = source.Materials.at(slot);
                    const auto materialName = "BistroVisibleGlass_" + original->Name;
                    auto* glass = render.GetMaterialByName(materialName);
                    if (!glass) glass = render.CreateMaterial(materialName);
                    glass->Diffuse = original->Diffuse;
                    glass->Normal = original->Normal;
                    glass->Specular = original->Specular;
                    glass->RoughnessMap = original->RoughnessMap;
                    glass->Emissive = original->Emissive;
                    glass->Displacement = original->Displacement;
                    glass->HasNormalMap = original->HasNormalMap;
                    glass->HasSpecularMap = original->HasSpecularMap;
                    glass->HasRoughnessMap = original->HasRoughnessMap;
                    glass->HasEmissiveMap = original->HasEmissiveMap;
                    glass->Metallic = original->Metallic;
                    glass->Roughness = original->Roughness;
                    glass->SpecularColor = original->SpecularColor;
                    glass->EmissiveColor = original->EmissiveColor + Vector3(.04f, .12f, .16f);
                    glass->Opacity = BistroPhysics::GlassOpacity.GetValue();
                    glass->Type = MaterialType::Transparent;
                    glass->DirtyFlag = true;
                    materials.at(slot) = glass;
                }
            }
            entity.AddComponent<StaticMeshRenderComponent>(source.MeshHandler, materials, indices).ReflectionDynamic = motion == PhysicsMotionType::Dynamic;
        }
        auto& body = entity.AddComponent<RigidBodyComponent>();
        body.MotionType = motion;
        body.ContinuousCollision = motion == PhysicsMotionType::Dynamic;
        try { entity.AddComponent<ColliderComponent>(shape); }
        catch (const std::exception& error) { throw std::runtime_error(name + ": " + error.what()); }
        if (waves && motion == PhysicsMotionType::Dynamic)
        {
            waves->Register(entity.GetId());
            // Configure the speed cap during the initial wait too; a zero impulse adds no energy.
            if (physics) physics->AddBodyVelocityImpulse(world, body.Body, Vector3::Zero, BistroWaves::MaxSpeed);
        }
    };
    if (!partition.Interior)
    {
        std::vector<uint32_t> exterior, entrance;
        for (const auto index : partition.StaticSubMeshes)
        {
            auto node = mesh.GetSubMesh(index).SourceNodeName;
            std::transform(node.begin(), node.end(), node.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
            auto& group = node == "paris_building_01_paris_building_01_bottom_121" ||
                node == "paris_building_01_paris_building_01_bottom_123" ? entrance : exterior;
            group.push_back(index);
        }
        create(sceneName, exterior, BistroStaticCollision::BuildWalkway(), PhysicsMotionType::Static);
        if (!entrance.empty())
        {
            auto door = ecs.CreateEntity();
            door.AddComponent<NameComponent>("BistroEntranceDoor");
            door.AddComponent<TransformComponent>(Vector3::Zero);
            door.AddComponent<StaticMeshRenderComponent>(source.MeshHandler, source.Materials, entrance).ReflectionDynamic = true;
        }
        return;
    }

    const bool simulate = BistroPhysics::SimulationEnabled.GetValue();
    const size_t count = categorized ? partition.Props.size() : BistroPhysics::GetCount(partition, simulate ? BistroPhysics::CountScale.GetValue() : 1.0);
    if (ecs.GetPool<RigidBodyComponent>().Size() + count + 1 > BistroPhysics::MaxBodies)
        throw std::length_error("Combined Bistro physics body budget exceeded");
    struct BakeColliders final : ParallelJob
    {
        BakeColliders(const Engine::Core::Mesh& mesh, const BistroPropPartition& partition, bool categorized)
            : Mesh(mesh), Partition(partition), Props(partition.Props.size()), Categorized(categorized) {}
        void Execute(size_t index) override
        {
            Props[index] = Categorized ? BistroPhysics::CategoryShape(Mesh, Partition.Props[index]) : BistroPhysics::PropShape(Mesh, Partition.Props[index]);
        }
        const Engine::Core::Mesh& Mesh;
        const BistroPropPartition& Partition;
        std::vector<PhysicsShapeDesc> Props;
        bool Categorized;
    } baked(mesh, partition, categorized);
    if (!partition.Props.empty())
    {
        const auto prepared = baked.Schedule(jobs, partition.Props.size(), 1);
        if (auto* app = dynamic_cast<App*>(BenchmarkEngine::GetInstance()); app && &app->GetJobs() == &jobs)
            app->WaitForLoading(prepared);
        else prepared.Complete();
    }
    const PhysicsShapeDesc room = BistroStaticCollision::BuildRoom();
    StaticMeshRenderComponent staticSource(source.MeshHandler, source.Materials, partition.StaticSubMeshes);
    if (architecture && architecture(staticSource)) create(sceneName + "_CollisionShell", {}, room, PhysicsMotionType::Static);
    else create(sceneName, partition.StaticSubMeshes, room, PhysicsMotionType::Static);
    std::vector<DirectX::BoundingBox> bounds;
    bounds.reserve(partition.Props.size());
    for (const auto& prop : partition.Props) bounds.push_back(BistroPhysics::Bounds(mesh, prop.SubMeshIndices));
    auto dynamic = categorized ? std::vector<bool>(partition.Props.size(), false) :
        BistroPhysics::SelectDynamicProps(partition, bounds, BistroPhysics::ActiveTablewareFraction.GetValue(), simulate);
    if (categorized)
    {
        std::array<size_t, static_cast<unsigned>(BistroPropCategory::Count)> counts{};
        for (size_t i = 0; i < partition.Props.size(); ++i)
        {
            dynamic[i] = simulate && BistroPropDynamic(partition.Props[i].Category, quality);
            ++counts[static_cast<unsigned>(partition.Props[i].Category)];
        }
        for (int preset = 0; preset <= 4; ++preset)
            for (unsigned category = 1; category < counts.size(); ++category)
            {
                const auto active = BistroPropDynamic(static_cast<BistroPropCategory>(category), preset) ? counts[category] : 0;
                Log::Info("BistroPhysics", "inventory quality=", preset, "; category=", BistroPropCategoryName(static_cast<BistroPropCategory>(category)),
                    "; static=", counts[category]-active, "; dynamic=", active, "; wave=", active);
            }
        for (auto name : partition.StaticObjects)
        {
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            bool known = false;
            for (const auto* token : {"building", "curtain", "ceiling_light", "wall_light", "ceilingfan", "painting", "radiator", "beertap"})
                known |= name.find(token) != std::string::npos;
            if (!known) Log::Info("BistroPhysics", "unclassified static object=", name);
        }
        Log::Info("BistroPhysics", "architecture groups=", partition.StaticObjects.size(), "; architecture bodies=1; requested quality=",
            BistroPhysics::Quality.GetValue(), "; effective=", simulate ? quality : 0, "; CountScale/fraction ignored");
    }
    for (size_t index = 0; index < partition.Props.size(); ++index)
    {
        const auto& prop = partition.Props[index];
        // Imported vertices retain their baked positions. Jolt shifts the body origin as it rotates about shape COM.
        create(prop.Name, prop.SubMeshIndices, baked.Props[index],
            dynamic[index] ? PhysicsMotionType::Dynamic : PhysicsMotionType::Static);
    }
    if (count == partition.Props.size()) return;
    if (!physics) throw std::logic_error("Spawn copies require physics placement queries");
    constexpr float floorHeight = BistroStaticCollision::FloorHeight;
    constexpr float ceilingHeight = BistroStaticCollision::CeilingHeight;
    auto roomBounds = mesh.GetSubMesh(0).Bounds;
    for (const auto& part : mesh.GetSubMeshes()) DirectX::BoundingBox::CreateMerged(roomBounds, roomBounds, part.Bounds);
    const auto roomMinimum = Vector3(roomBounds.Center)-Vector3(roomBounds.Extents);
    const auto roomMaximum = Vector3(roomBounds.Center)+Vector3(roomBounds.Extents);
    std::vector<Vector3> occupied;
    for (const auto& b : bounds) occupied.emplace_back(b.Center);
    const auto footprint = BistroPhysics::SpawnFootprint(std::move(occupied));
    const auto& repeatable=partition.Tableware;
    if (repeatable.empty()) throw std::runtime_error("Bistro count scaling requires indoor tableware");
    std::vector<size_t> copies(count-partition.Props.size());
    for (size_t i=0; i<copies.size(); ++i) copies[i]=i;
    const auto sourceIndex = [&](size_t copy) { return repeatable[copy%repeatable.size()]; };
    std::stable_sort(copies.begin(),copies.end(),[&](size_t a,size_t b)
    {
        const auto& x=bounds[sourceIndex(a)].Extents;
        const auto& y=bounds[sourceIndex(b)].Extents;
        return x.x*x.y*x.z > y.x*y.y*y.z;
    });
    const auto halton = [](size_t index, size_t base)
    {
        float value=0, fraction=1;
        while (index) { fraction/=static_cast<float>(base); value+=fraction*static_cast<float>(index%base); index/=base; }
        return value;
    };
    size_t attempts=0;
    for (const auto copy : copies)
    {
        const size_t index=sourceIndex(copy);
        const auto& b=bounds[index];
        const Vector3 minimum(roomMinimum.x+b.Extents.x+1, floorHeight+b.Extents.y+1, roomMinimum.z+b.Extents.z+1);
        const Vector3 maximum(roomMaximum.x-b.Extents.x-1, ceilingHeight-b.Extents.y-1, roomMaximum.z-b.Extents.z-1);
        PhysicsBodyDesc query; query.Shape=baked.Props[index];
        bool found=false;
        for (size_t attempt=0; attempt<16384; ++attempt)
        {
            const size_t sample=++attempts;
            const Vector3 center=minimum+(maximum-minimum)*Vector3(halton(sample,2),halton(sample,3),halton(sample,5));
            if (!BistroPhysics::FitsFootprint(footprint,center,Vector3(b.Extents)+Vector3(2))) continue;
            query.Position=center-Vector3(b.Center);
            if (physics->OverlapsBody(world,query)) continue;
            PhysicsRayHit floor, ceiling;
            if (!physics->RayCastStatic(world,center,{0,floorHeight-center.y-2,0},floor) || floor.Normal.y<.5f ||
                !physics->RayCastStatic(world,center,{0,ceilingHeight-center.y+2,0},ceiling) || ceiling.Normal.y>-.5f) continue;
            const auto& prop=partition.Props[index];
            create(prop.Name+"_PhysicsCopy_"+std::to_string(copy),prop.SubMeshIndices,baked.Props[index],
                PhysicsMotionType::Dynamic,query.Position);
            found=true;
            break;
        }
        if (!found) throw std::runtime_error("Cannot place indoor Bistro tableware copy: "+partition.Props[index].Name);
    }
}
