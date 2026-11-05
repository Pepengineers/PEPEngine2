#include "App.Base/Physics/CollisionGeometry.h"
#include "App.Base/Physics/JoltPhysicsBackend.h"
#include "Engine.Core/Jobs/JobSystem.h"
#include <windows.h>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>

using namespace DirectX;
using namespace DirectX::SimpleMath;
using namespace Engine::Core;

static void Require(bool value, const char* message)
{ if (!value) throw std::runtime_error(message); }

static SubMesh Box(Vector3 center, Vector3 half, Quaternion rotation = Quaternion::Identity)
{
    SubMesh part;
    for (const auto& p : {Vector3(-1,-1,-1), Vector3(1,-1,-1), Vector3(1,1,-1), Vector3(-1,1,-1),
        Vector3(-1,-1,1), Vector3(1,-1,1), Vector3(1,1,1), Vector3(-1,1,1)})
    {
        Vertex vertex;
        vertex.Position = Vector3::Transform(p * half, rotation) + center;
        part.Vertices.push_back(vertex);
    }
    part.Indices = {0,2,1,0,3,2,4,5,6,4,6,7,0,4,7,0,7,3,1,2,6,1,6,5,0,1,5,0,5,4,3,7,6,3,6,2};
    return part;
}

static PhysicsCompoundChild SinglePrimitive(const PhysicsShapeDesc& shape)
{
    const auto* compound = std::get_if<CompoundShapeDesc>(&shape);
    Require(compound && compound->Geometry && compound->Geometry->Children.size() == 1, "Simple mesh was not fitted automatically");
    return compound->Geometry->Children.front();
}

static SubMesh Bottle(bool neck)
{
    SubMesh part;
    const std::vector<Vector2> profile = neck ? std::vector<Vector2>{{0,10},{40,10},{40,4},{60,4}}
        : std::vector<Vector2>{{0,10},{40,10}};
    constexpr uint32_t slices = 32;
    for (const auto& ring : profile) for (uint32_t slice = 0; slice < slices; ++slice)
    {
        const float angle = XM_2PI * slice / slices;
        Vertex vertex; vertex.Position = {ring.y * std::cos(angle), ring.x, ring.y * std::sin(angle)};
        part.Vertices.push_back(vertex);
    }
    for (uint32_t ring = 0; ring + 1 < profile.size(); ++ring) for (uint32_t slice = 0; slice < slices; ++slice)
    {
        const uint32_t a = ring*slices+slice, b = a+slices, next = ring*slices+(slice+1)%slices;
        part.Indices.insert(part.Indices.end(), {a,b,next,next,b,next+slices});
    }
    for (uint32_t end = 0; end < 2; ++end)
    {
        const uint32_t ring = end ? static_cast<uint32_t>(profile.size()-1) : 0;
        const uint32_t center = static_cast<uint32_t>(part.Vertices.size());
        Vertex vertex; vertex.Position = {0,profile[ring].x,0}; part.Vertices.push_back(vertex);
        for (uint32_t slice = 0; slice < slices; ++slice)
        {
            const uint32_t a=ring*slices+slice, b=ring*slices+(slice+1)%slices;
            part.Indices.insert(part.Indices.end(), end ? std::initializer_list<uint32_t>{center,b,a} : std::initializer_list<uint32_t>{center,a,b});
        }
    }
    return part;
}

static void CheckNearConvex()
{
    const auto cylinder = SinglePrimitive(BuildDynamicCollision(Mesh({Bottle(false)},{}),{0},0.25f));
    Require(std::holds_alternative<CylinderShapeDesc>(cylinder.Shape), "Cylinder mesh was not fitted automatically");
    const auto bottle = BuildDynamicCollision(Mesh({Bottle(true)},{}),{0},1.0f);
    const auto* hull = std::get_if<ConvexCompoundShapeDesc>(&bottle);
    const size_t count = hull ? hull->Geometry->Hulls.size() : std::get<CompoundShapeDesc>(bottle).Geometry->Children.size();
    Require(count > 1 && count <= 12, "Bottle shoulder exceeding the error was filled or exceeded its hull budget");
    auto outer = Bottle(false), inner = Bottle(false);
    for (auto& vertex : outer.Vertices) vertex.Position *= 0.2f;
    for (auto& vertex : inner.Vertices) vertex.Position = vertex.Position * 0.18f + Vector3(0,0.4f,0);
    for (size_t i = 0; i < inner.Indices.size(); i += 3) std::swap(inner.Indices[i+1], inner.Indices[i+2]);
    const auto shell = BuildDynamicCollision(Mesh({outer,inner},{}),{0,1},1.0f);
    const auto* shellHull = std::get_if<ConvexCompoundShapeDesc>(&shell);
    const size_t shellParts = shellHull ? shellHull->Geometry->Hulls.size() : std::get<CompoundShapeDesc>(shell).Geometry->Children.size();
    Require(shellParts == 1, "A hollow shell following its convex envelope was unnecessarily decomposed");
    std::cout << "Automatic cylinder fitting, bounded bottle shoulder and single-part hollow shell baking passed\n";
}

static SubMesh RoundMesh(bool capsule)
{
    SubMesh part;
    constexpr unsigned rings = 16, slices = 32;
    for (unsigned i = 0; i <= rings; ++i)
    {
        const float latitude = XM_PI * i / rings;
        for (unsigned j = 0; j < slices; ++j)
        {
            const float longitude = XM_2PI * j / slices;
            Vertex vertex;
            vertex.Position = {10 * std::sin(latitude) * std::cos(longitude), 10 * std::cos(latitude), 10 * std::sin(latitude) * std::sin(longitude)};
            if (capsule) vertex.Position.y += i <= rings/2 ? 15.f : -15.f;
            part.Vertices.push_back(vertex);
        }
    }
    for (unsigned i = 0; i < rings; ++i) for (unsigned j = 0; j < slices; ++j)
    {
        const auto a = i * slices + j, b = i * slices + (j + 1) % slices;
        part.Indices.insert(part.Indices.end(), {a, a+slices, b, b, a+slices, b+slices});
    }
    return part;
}

static void CheckPrimitiveFitting(JobSystem& jobs)
{
    const Vector3 center(1000, 200, -400);
    const auto rotation = Quaternion::CreateFromYawPitchRoll(0.6f, 0.25f, -0.3f);
    const Mesh mesh({Box(center, {20, 10, 5}, rotation)}, {});
    const auto shape = BuildDynamicCollision(mesh, {0}, 0.05f);
    Require(std::holds_alternative<BoxShapeDesc>(SinglePrimitive(shape).Shape), "Rotated box did not use a primitive");
    JoltPhysicsBackend backend;
    PhysicsWorldSettings settings; settings.WorldUnitsPerMeter = 100;
    backend.Initialize(jobs, settings);
    PhysicsBodyDesc body; body.Shape = shape;
    Require(backend.CreateBody(body).IsValid(), "Fitted primitive could not be cooked");
    const auto surface = center + Vector3::Transform({0,10,0}, rotation);
    const auto up = Vector3::Transform(Vector3::UnitY, rotation);
    PhysicsRayHit hit;
    Require(backend.RayCastStatic(surface + up * 20, up * -40, hit) && Vector3::Distance(surface, hit.Position) < 0.1f,
        "Automatic primitive changed the baked pivot or rotation");
    Require(std::holds_alternative<SphereShapeDesc>(SinglePrimitive(BuildDynamicCollision(Mesh({RoundMesh(false)}, {}), {0}, 0.1f)).Shape),
        "Round geometry did not use a sphere");
    Require(std::holds_alternative<CapsuleShapeDesc>(SinglePrimitive(BuildDynamicCollision(Mesh({RoundMesh(true)}, {}), {0}, 0.1f)).Shape),
        "Elongated round geometry did not use a capsule");
    std::cout << "Automatic box/sphere/capsule fitting and translated/rotated baked pivots passed\n";
}

static std::set<std::filesystem::path> CacheFiles()
{
    std::set<std::filesystem::path> result;
    const auto folder = std::filesystem::path(SOLUTION_DIR) / "Build/AssetCache/Collision";
    if (std::filesystem::exists(folder))
        for (const auto& file : std::filesystem::directory_iterator(folder)) if (file.path().extension() == ".mesh") result.insert(file.path());
    return result;
}

static void CheckStaticAndCache(JobSystem& jobs)
{
    SubMesh plane;
    const Vector3 offset(float(GetCurrentProcessId()) * 10, 37, -1000);
    constexpr unsigned size = 24;
    for (unsigned z=0; z<=size; ++z) for (unsigned x=0; x<=size; ++x)
    {
        Vertex vertex; vertex.Position = offset + Vector3(float(x)*10, 0, float(z)*10); plane.Vertices.push_back(vertex);
    }
    for (unsigned z=0; z<size; ++z) for (unsigned x=0; x<size; ++x)
    {
        if (x>=10 && x<14 && z>=10 && z<14) continue;
        const auto a=z*(size+1)+x;
        plane.Indices.insert(plane.Indices.end(), {a,a+size+1,a+1,a+1,a+size+1,a+size+2});
    }
    const Mesh mesh({plane}, {});
    const auto before = CacheFiles();
    const auto collision = BuildStaticCollision(mesh, {0}, 0.1f);
    Require(collision.Geometry->Triangles.size() * 3 < plane.Indices.size()/2, "QEM did not remove dense coplanar triangles");
    const auto after = CacheFiles();
    std::vector<std::filesystem::path> created;
    for (const auto& path : after) if (!before.count(path)) created.push_back(path);
    Require(created.size() == 1, "Static collision bake did not publish one derived cache");
    const auto stamp = std::filesystem::last_write_time(created.front());
    const auto warm = BuildStaticCollision(mesh, {0}, 0.1f);
    Require(warm.Geometry->Triangles == collision.Geometry->Triangles && warm.Geometry->Vertices == collision.Geometry->Vertices &&
        std::filesystem::last_write_time(created.front()) == stamp, "Warm collision load rebuilt or changed the derived mesh");
    BuildStaticCollision(mesh, {0}, 0.2f);
    Require(CacheFiles().size() == after.size()+1, "Changing bake parameters reused a stale collision cache");
    { std::ofstream corrupt(created.front(), std::ios::binary|std::ios::trunc); corrupt << "broken"; }
    const auto rebuilt = BuildStaticCollision(mesh, {0}, 0.1f);
    Require(rebuilt.Geometry->Triangles == collision.Geometry->Triangles, "Corrupt derived data did not rebuild from source");
    JoltPhysicsBackend backend;
    PhysicsWorldSettings settings; settings.WorldUnitsPerMeter = 100;
    backend.Initialize(jobs, settings);
    PhysicsBodyDesc body; body.Shape = collision;
    Require(backend.CreateBody(body).IsValid(), "Simplified plane could not be cooked");
    PhysicsRayHit hit;
    Require(backend.RayCastStatic(offset+Vector3(50,100,50), {0,-200,0}, hit) && std::abs(hit.Position.y-offset.y)<0.01f,
        "Simplification moved a supporting plane");
    Require(!backend.RayCastStatic(offset+Vector3(120,100,120), {0,-200,0}, hit) &&
        !backend.RayCastStatic(offset+Vector3(-10,100,120), {0,-200,0}, hit), "Simplification filled a hole or extended the boundary");
    bool rejected=false;
    try { BuildStaticCollision(mesh,{0},NAN); } catch(const std::invalid_argument&){rejected=true;}
    Require(rejected,"Nonfinite collision error accepted");
    std::cout << "Static QEM reduction, support/hole/boundary preservation, warm cache, invalidation and corrupt-cache rebuild passed\n";
}

static void CheckConcaveDecomposition(JobSystem& jobs)
{
    const Vector3 offset(1000,0,-400);
    const auto rotation = Quaternion::CreateFromAxisAngle(Vector3::UnitY, 0.4f);
    std::vector<SubMesh> parts;
    const auto add=[&](Vector3 center,Vector3 half){parts.push_back(Box(Vector3::Transform(center,rotation)+offset,half,rotation));};
    add({0,40,0},{25,3,20});
    for(float x:{-20.f,20.f}) for(float z:{-15.f,15.f}) add({x,20,z},{2,20,2});
    add({0,65,-18},{25,22,2});
    const Mesh mesh(parts,{});
    const auto shape=BuildDynamicCollision(mesh,{0,1,2,3,4,5},0.25f,12);
    if(const auto* convex=std::get_if<ConvexCompoundShapeDesc>(&shape))
    {
        Require(convex->Geometry->Hulls.size()>1 && convex->Geometry->Hulls.size()<=12,"Concave decomposition missed its hull budget");
        for(const auto& hull:convex->Geometry->Hulls) Require(hull.size()<=32,"Convex hull exceeded its vertex budget");
    }
    else Require(std::get<CompoundShapeDesc>(shape).Geometry->Children.size()>1,"Concave chair became one filled primitive");
    JoltPhysicsBackend backend;
    PhysicsWorldSettings settings;settings.WorldUnitsPerMeter=100;
    backend.Initialize(jobs,settings);
    PhysicsBodyDesc body;body.Shape=shape;
    Require(backend.CreateBody(body).IsValid(),"Automatic chair collision could not be cooked");
    Require(!backend.OverlapsBox(offset+Vector3(0,10,0),{3,3,3}),"Automatic decomposition filled the space under the seat");
    Require(backend.OverlapsBox(offset+Vector3::Transform({20,10,15},rotation),{1,1,1}),"Automatic decomposition lost a chair leg");
    PhysicsRayHit hit;
    Require(backend.RayCastStatic(offset+Vector3(0,100,0),{0,-120,0},hit) && std::abs(hit.Position.y-43)<1,
        "Automatic decomposition lost the seat support");
    const auto warm=BuildDynamicCollision(mesh,{0,1,2,3,4,5},0.25f,12);
    Require(warm.index()==shape.index(),"Warm convex cache changed collision representation");
    std::cout << "Automatic bounded concave decomposition preserved seat, legs and central void\n";
}

static void CheckContactGaps(JobSystem& jobs)
{
    for (const float gap : {4.f, 20.f})
    {
        const float half = (100-gap)/4;
        const Mesh mesh({Box({0,-(half+gap/2),0},{50,half,50}), Box({0,half+gap/2,0},{50,half,50})},{});
        JoltPhysicsBackend backend;
        backend.Initialize(jobs, PhysicsWorldSettings{});
        PhysicsBodyDesc body; body.Shape=BuildDynamicCollision(mesh,{0,1},1);
        Require(backend.CreateBody(body).IsValid(),"Slab collision could not be cooked");
        Require(!backend.OverlapsBox({0,0,0},{1,1,1}),"Automatic collision filled a gap between nearly solid slabs");
        Require(backend.OverlapsBox({0,30,0},{1,1,1}),"Automatic collision removed a slab");
    }
    // An off-center opening misses the old hull centroid/edge-midpoint samples.
    const Mesh frame({Box({-15,0,0},{35,5,50}), Box({40,0,0},{10,5,50}),
        Box({25,0,-27.5f},{5,5,22.5f}), Box({25,0,27.5f},{5,5,22.5f})},{});
    JoltPhysicsBackend backend;
    backend.Initialize(jobs, PhysicsWorldSettings{});
    PhysicsBodyDesc body; body.Shape=BuildDynamicCollision(frame,{0,1,2,3},1);
    Require(backend.CreateBody(body).IsValid(),"Frame collision could not be cooked");
    Require(!backend.OverlapsBox({25,0,0},{1,1,1}),"Automatic collision filled an off-center through-hole");
    Require(backend.OverlapsBox({-20,0,0},{1,1,1}),"Automatic collision removed the frame");
    std::cout << "High-volume slab gaps and off-center frame opening preserved\n";
}

int main()
{
    try
    {
        std::cout<<std::unitbuf;
        JobSystem jobs(4);
        CheckContactGaps(jobs);CheckPrimitiveFitting(jobs);CheckNearConvex();CheckStaticAndCache(jobs);CheckConcaveDecomposition(jobs);
        return 0;
    }
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
