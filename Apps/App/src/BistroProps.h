#pragma once

#include "Engine.Core/Types/MeshTypes.h"
#include "App.Base/ECS/World.h"
#include "App.Base/Modules/PhysicsModule.h"
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

struct BistroPropGroup
{
    std::string Name;
    std::vector<std::uint32_t> SubMeshIndices;
};

struct BistroPropPartition
{
    std::vector<std::uint32_t> StaticSubMeshes;
    std::vector<BistroPropGroup> Props;
    std::vector<size_t> Tableware;
    bool Interior = true;
};

inline bool IsBistroTableware(const std::string& lower)
{
    for (const auto* family : {"liquorbottle", "glass_water", "glass_wine", "glass_beer", "glass_detailed",
        "plate", "cutlery", "napkinholder", "placemat", "wickerbasket", "coaster"})
        if (lower.find(family) != std::string::npos) return true;
    return false;
}

inline BistroPropPartition PartitionBistroProps(const Engine::Core::Mesh& mesh, bool interior = true)
{
    BistroPropPartition result;
    result.Interior = interior;
    std::unordered_map<std::string, size_t> groups;
    const auto& parts = mesh.GetSubMeshes();
    for (size_t index = 0; index < parts.size(); ++index)
    {
        const auto& name = parts[index].SourceNodeName;
        std::string lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (!interior || (!IsBistroTableware(lower) && lower.find("chair") == std::string::npos && lower.find("table") == std::string::npos))
        {
            result.StaticSubMeshes.push_back(static_cast<std::uint32_t>(index));
            continue;
        }
        auto objectName = name;
        const auto section = objectName.find("_Mesh_");
        if (section != std::string::npos && section + 8 <= objectName.size()) objectName.erase(section, 8);
        const auto inserted = groups.emplace(objectName, result.Props.size());
        if (inserted.second)
        {
            if (IsBistroTableware(lower)) result.Tableware.push_back(result.Props.size());
            result.Props.push_back({ objectName, {} });
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
    inline AutoConsoleVariable<double> CountScale(L"App.Physics.CountScale", 1.0,
        L"Indoor tableware count, 0..16: >1 multiplies bottles/tableware only; original furniture is independent; reload to apply",
        EConsoleVariableFlags::Default, [](double value) { return value >= 0 && value <= 16; });

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
        else DirectX::BoundingOrientedBox::CreateFromPoints(box, points.size(), points.data(), sizeof(Vector3));
        // Thin furniture surfaces still need a solid collision thickness (centimeters).
        const Vector3 half((std::max)(box.Extents.x, 0.5f), (std::max)(box.Extents.y, 0.5f), (std::max)(box.Extents.z, 0.5f));
        return {BoxShapeDesc{half}, Vector3(box.Center) + origin, Quaternion(box.Orientation)};
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
    const BistroPropPartition& partition, JobSystem& jobs, PhysicsModule* physics = nullptr, BistroWaves::Controller* waves = nullptr)
{
    auto& ecs = world.GetECS();
    const auto create = [&](const std::string& name, const std::vector<uint32_t>& indices,
        const PhysicsShapeDesc& shape, PhysicsMotionType motion, Vector3 offset = Vector3::Zero)
    {
        auto entity = ecs.CreateEntity();
        entity.AddComponent<NameComponent>(name);
        entity.AddComponent<TransformComponent>(offset);
        if (!indices.empty())
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
    if (!partition.Interior)
    {
        create(sceneName, partition.StaticSubMeshes, BistroStaticCollision::BuildWalkway(), PhysicsMotionType::Static);
        return;
    }

    const size_t count = BistroPhysics::GetCount(partition, BistroPhysics::CountScale.GetValue());
    if (ecs.GetPool<RigidBodyComponent>().Size() + count + 1 > BistroPhysics::MaxBodies)
        throw std::length_error("Combined Bistro physics body budget exceeded");
    struct BakeColliders final : ParallelJob
    {
        BakeColliders(const Engine::Core::Mesh& mesh, const BistroPropPartition& partition)
            : Mesh(mesh), Partition(partition), Props(partition.Props.size()) {}
        void Execute(size_t index) override
        {
            Props[index] = BistroPhysics::PropShape(Mesh, Partition.Props[index]);
        }
        const Engine::Core::Mesh& Mesh;
        const BistroPropPartition& Partition;
        std::vector<PhysicsShapeDesc> Props;
    } baked(mesh, partition);
    if (!partition.Props.empty())
    {
        const auto prepared = baked.Schedule(jobs, partition.Props.size(), 1);
        if (auto* app = dynamic_cast<App*>(BenchmarkEngine::GetInstance()); app && &app->GetJobs() == &jobs)
            app->WaitForLoading(prepared);
        else prepared.Complete();
    }
    const PhysicsShapeDesc room = BistroStaticCollision::BuildRoom();
    create(sceneName, partition.StaticSubMeshes, room, PhysicsMotionType::Static);
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
