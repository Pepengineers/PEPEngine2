#pragma once

#include "Engine.Core/Types/MeshTypes.h"
#include "App.Base/ECS/World.h"
#include "App.Base/Modules/PhysicsModule.h"
#include "Common/ConsoleVariables.h"
#include "BistroWaves.h"
#include "App.Base/Physics/CollisionGeometry.h"
#include "Engine.Core/Jobs/JobSystem.h"
#include "App.Base/App.h"
#include <cmath>
#include <stdexcept>
#include <algorithm>
#include <cctype>
#include <unordered_map>

struct BistroPropGroup
{
    std::string Name;
    std::vector<std::uint32_t> SubMeshIndices;
};

struct BistroPropPartition
{
    std::vector<std::uint32_t> StaticSubMeshes;
    std::vector<BistroPropGroup> Props;
};

inline BistroPropPartition PartitionBistroProps(const Engine::Core::Mesh& mesh)
{
    BistroPropPartition result;
    std::unordered_map<std::string, size_t> groups;
    const auto& parts = mesh.GetSubMeshes();
    for (size_t index = 0; index < parts.size(); ++index)
    {
        const auto& name = parts[index].SourceNodeName;
        std::string lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lower.find("liquorbottle") == std::string::npos && lower.find("chair") == std::string::npos && lower.find("table") == std::string::npos)
        {
            result.StaticSubMeshes.push_back(static_cast<std::uint32_t>(index));
            continue;
        }
        auto objectName = name;
        const auto section = objectName.find("_Mesh_");
        if (section != std::string::npos && section + 8 <= objectName.size()) objectName.erase(section, 8);
        const auto inserted = groups.emplace(objectName, result.Props.size());
        if (inserted.second) result.Props.push_back({ objectName, {} });
        result.Props[inserted.first->second].SubMeshIndices.push_back(static_cast<std::uint32_t>(index));
    }
    return result;
}

namespace BistroPhysics
{
    inline constexpr uint32_t MaxBodies = 8192;
    inline AutoConsoleVariable<double> CountScale(L"App.Physics.CountScale", 1.0,
        L"Bistro spawn scale, 0..16: 0..1 keeps every original, >1 adds ceil(base*scale)-base copies; reload scene to apply",
        EConsoleVariableFlags::Default, [](double value) { return value >= 0 && value <= 16; });

    inline size_t GetCount(size_t base, double scale)
    {
        if (!std::isfinite(scale) || scale < 0 || scale > 16 || base > MaxBodies - 2)
            throw std::invalid_argument("Invalid Bistro physics spawn budget");
        const double count = std::ceil(static_cast<double>(base) * (std::max)(1.0, scale));
        if (count > MaxBodies - 2) throw std::length_error("Bistro physics body budget exceeded");
        return static_cast<size_t>(count);
    }

    inline PhysicsWorldSettings Settings()
    {
        // Use a 1 mm contact tolerance for centimeter-wide bottles.
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

    inline TriangleMeshShapeDesc StaticShape(const Engine::Core::Mesh& mesh, const std::vector<uint32_t>& indices)
    {
        return BuildStaticCollision(mesh, indices, 1.0f);
    }

    inline PhysicsCompoundChild BoxProxy(std::vector<Vector3> points)
    {
        if (points.empty()) throw std::invalid_argument("Empty Bistro box proxy");
        const auto origin = points.front();
        for (auto& point : points)
        {
            point -= origin;
            if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z))
                throw std::invalid_argument("Nonfinite Bistro box proxy");
        }
        DirectX::BoundingOrientedBox box;
        DirectX::BoundingOrientedBox::CreateFromPoints(box, points.size(), points.data(), sizeof(Vector3));
        // Thin furniture surfaces still need a solid collision thickness (centimeters).
        const Vector3 half((std::max)(box.Extents.x, 0.5f), (std::max)(box.Extents.y, 0.5f), (std::max)(box.Extents.z, 0.5f));
        return {BoxShapeDesc{half}, Vector3(box.Center) + origin, Quaternion(box.Orientation)};
    }

    inline PhysicsShapeDesc PropShape(const Engine::Core::Mesh& mesh, const BistroPropGroup& prop)
    {
        std::string lower = prop.Name;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lower.find("liquorbottle") != std::string::npos)
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
        auto compound = std::make_shared<PhysicsCompound>();
        if (const auto* convex = std::get_if<ConvexCompoundShapeDesc>(&baked))
        {
            for (const auto& hull : convex->Geometry->Hulls) compound->Children.push_back(BoxProxy(hull));
        }
        else if (const auto* mixed = std::get_if<CompoundShapeDesc>(&baked))
        {
            for (const auto& child : mixed->Geometry->Children)
                if (const auto* convex = std::get_if<ConvexCompoundShapeDesc>(&child.Shape))
                    for (const auto& hull : convex->Geometry->Hulls)
                    {
                        auto box = BoxProxy(hull);
                        box.Position = Vector3::Transform(box.Position, child.Rotation) + child.Position;
                        box.Rotation *= child.Rotation;
                        compound->Children.push_back(box);
                    }
                else compound->Children.push_back(child);
        }
        else return baked;
        return CompoundShapeDesc{compound};
    }
}

inline void CreateBistroPropEntities(World& world, const std::string& sceneName,
    const StaticMeshRenderComponent& source, const Engine::Core::Mesh& mesh,
    const BistroPropPartition& partition, JobSystem& jobs, PhysicsModule* physics = nullptr, BistroWaves::Controller* waves = nullptr)
{
    auto& ecs = world.GetECS();
    const size_t count = BistroPhysics::GetCount(partition.Props.size(), BistroPhysics::CountScale.GetValue());
    if (ecs.GetPool<RigidBodyComponent>().Size() + count + 1 > BistroPhysics::MaxBodies)
        throw std::length_error("Combined Bistro physics body budget exceeded");
    struct BakeColliders final : ParallelJob
    {
        BakeColliders(const Engine::Core::Mesh& mesh, const BistroPropPartition& partition)
            : Mesh(mesh), Partition(partition), Props(partition.Props.size()) {}
        void Execute(size_t index) override
        {
            if (index == 0)
            {
                if (!Partition.StaticSubMeshes.empty()) Static = BistroPhysics::StaticShape(Mesh, Partition.StaticSubMeshes);
            }
            else Props[index - 1] = BistroPhysics::PropShape(Mesh, Partition.Props[index - 1]);
        }
        const Engine::Core::Mesh& Mesh;
        const BistroPropPartition& Partition;
        TriangleMeshShapeDesc Static;
        std::vector<PhysicsShapeDesc> Props;
    } baked(mesh, partition);
    const auto prepared = baked.Schedule(jobs, partition.Props.size() + 1, 1);
    if (auto* app = dynamic_cast<App*>(BenchmarkEngine::GetInstance()); app && &app->GetJobs() == &jobs)
        app->WaitForLoading(prepared);
    else prepared.Complete();
    const auto create = [&](const std::string& name, const std::vector<uint32_t>& indices,
        const PhysicsShapeDesc& shape, PhysicsMotionType motion, Vector3 offset = Vector3::Zero)
    {
        auto entity = ecs.CreateEntity();
        entity.AddComponent<NameComponent>(name);
        entity.AddComponent<TransformComponent>(offset);
        entity.AddComponent<StaticMeshRenderComponent>(source.MeshHandler, source.Materials, indices).ReflectionDynamic = motion == PhysicsMotionType::Dynamic;
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
    if (!partition.StaticSubMeshes.empty())
        create(sceneName, partition.StaticSubMeshes, baked.Static, PhysicsMotionType::Static);
    std::vector<DirectX::BoundingBox> bounds;
    for (size_t index = 0; index < partition.Props.size(); ++index)
    {
        const auto& prop = partition.Props[index];
        bounds.push_back(BistroPhysics::Bounds(mesh, prop.SubMeshIndices));
        // Imported vertices retain their baked positions. Jolt shifts the body origin as it rotates about shape COM.
        create(prop.Name, prop.SubMeshIndices, baked.Props[index], PhysicsMotionType::Dynamic);
    }
    if (count == partition.Props.size()) return;
    if (!physics) throw std::logic_error("Spawn copies require physics placement queries");
    Vector3 largest(1);
    for (const auto& b : bounds) largest = Vector3((std::max)(largest.x, b.Extents.x),
        (std::max)(largest.y, b.Extents.y), (std::max)(largest.z, b.Extents.z));
    const Vector3 step = largest * 2 + Vector3(10);
    // Use the occupied prop area, not the full city AABB whose corners can be empty sky.
    auto sceneBounds = bounds.front();
    for (size_t i=1; i<bounds.size(); ++i) DirectX::BoundingBox::CreateMerged(sceneBounds,sceneBounds,bounds[i]);
    const Vector3 minimum = Vector3(sceneBounds.Center) - Vector3(sceneBounds.Extents) + largest + Vector3(5);
    const Vector3 maximum = Vector3(sceneBounds.Center) + Vector3(sceneBounds.Extents) - largest - Vector3(5);
    // Start above floor-level furniture before trying higher layers.
    Vector3 candidate(minimum.x, minimum.y + step.y, minimum.z);
    size_t attempts = 0;
    for (size_t i = partition.Props.size(); i < count; ++i)
    {
        const size_t index = (i - partition.Props.size()) % partition.Props.size();
        const auto& b = bounds[index];
        while (true)
        {
            if (++attempts > count * 100) throw std::runtime_error("Cannot place Bistro physics copies");
            const auto center = candidate;
            candidate.x += b.Extents.x * 2 + 10;
            if (candidate.x > maximum.x) { candidate.x = minimum.x; candidate.z += step.z; }
            if (candidate.z > maximum.z) { candidate.z = minimum.z; candidate.y += step.y; }
            const float distance = center.y - (sceneBounds.Center.y-sceneBounds.Extents.y) + 100;
            const auto support = [&](const Vector3& point)
            {
                PhysicsRayHit hit;
                return physics->RayCastStatic(world,point,{0,-distance,0},hit) && hit.Normal.y>=0.5f;
            };
            bool supported = support(center);
            for (float x : {-b.Extents.x,b.Extents.x}) for (float z : {-b.Extents.z,b.Extents.z})
                supported = supported && support(center+Vector3(x,0,z));
            if (!supported) continue;
            if (physics->OverlapsBox(world, center, Vector3(b.Extents) + Vector3(2))) continue;
            const auto& prop = partition.Props[index];
            create(prop.Name + "_PhysicsCopy_" + std::to_string(i), prop.SubMeshIndices, baked.Props[index],
                PhysicsMotionType::Dynamic, center - Vector3(b.Center));
            break;
        }
    }
}
