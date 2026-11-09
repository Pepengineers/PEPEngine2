#include "App.Base/Physics/JoltJobSystem.h"
#include "App.Base/App.h"
#include "App.Base/Physics/JoltPhysicsBackend.h"
#include "App.Base/Modules/PhysicsModule.h"
#include "App.Base/Modules/SceneManagerModule.h"
#include "App.Base/Systems/DynamicBodyUpdateSystem.h"
#include "App.Base/Systems/KinematicBodyUpdateSystem.h"
#include "Engine.Core/BenchmarkEngine.h"
#include <array>
#include <atomic>
#include <cmath>
#include <iostream>
#include <limits>
#include <fstream>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

#undef AddJob

static void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

#ifdef _DEBUG
static void CheckCollisionGeometry(JobSystem& jobs)
{
    JoltPhysicsBackend physics;
    PhysicsWorldSettings settings;
    settings.WorldUnitsPerMeter = 100;
    physics.Initialize(jobs, settings);
    auto compound = std::make_shared<PhysicsCompound>();
    compound->Children.push_back({BoxShapeDesc{{20, 10, 10}}, {40, 0, 0}});
    compound->Children.push_back({BoxShapeDesc{{5, 5, 5}}, {70, 0, 0}});
    PhysicsBodyDesc body;
    body.Shape = CompoundShapeDesc{compound};
    body.Position = {100, 200, 300};
    body.Rotation = Quaternion::CreateFromAxisAngle(Vector3::UnitZ, XM_PIDIV2);
    Require(physics.CreateBody(body).IsValid(), "Static compound fixture failed");
    body.Shape = BoxShapeDesc{{10, 10, 10}};
    body.Position = {0, 300, 0};
    body.Rotation = Quaternion::Identity;
    body.MotionType = PhysicsMotionType::Dynamic;
    const auto dynamic = physics.CreateBody(body);
    Require(dynamic.IsValid(), "Dynamic collision fixture failed");

    std::vector<Vector3> statics, moving;
    physics.CollectCollisionTriangles(statics, moving);
    Require(!statics.empty() && !moving.empty(), "Both motion groups should have triangles");
    BoundingBox bounds;
    BoundingBox::CreateFromPoints(bounds, statics.size(), statics.data(), sizeof(Vector3));
    Require(Vector3::Distance(bounds.Center, {100, 247.5f, 300}) < 0.01f &&
        Vector3::Distance(bounds.Extents, {10, 27.5f, 10}) < 0.01f, "Collision geometry lost child offset, rotation or world units");
    BoundingBox::CreateFromPoints(bounds, moving.size(), moving.data(), sizeof(Vector3));
    Require(Vector3::Distance(bounds.Center, body.Position) < 0.01f, "Dynamic collision geometry used the wrong pose");
    physics.DestroyBody(dynamic);
    statics.clear(); moving.clear();
    physics.CollectCollisionTriangles(statics, moving);
    Require(!statics.empty() && moving.empty(), "Destroyed body remained in the collision view");
}
#endif

static void CheckTriangleSanitization(JobSystem& jobs)
{
    for (float units : {1.f, 100.f})
    {
        JoltPhysicsBackend physics;
        PhysicsWorldSettings settings;
        settings.WorldUnitsPerMeter = units;
        physics.Initialize(jobs, settings);
        auto mesh = std::make_shared<PhysicsTriangleMesh>();
        // Nonzero area passes Jolt's input check but has no safe margin for compression and rotation.
        mesh->Vertices = {{0,0,0}, {units,0,0}, {0.5f*units,0,0.000003f*units}};
        mesh->Triangles = {{0,1,2}};
        PhysicsBodyDesc body;
        body.Shape = TriangleMeshShapeDesc{mesh};
        Require(!physics.CreateBody(body).IsValid(), "Mesh cooker accepted a nearly collinear triangle");
        mesh = std::make_shared<PhysicsTriangleMesh>(*mesh);
        mesh->Vertices.insert(mesh->Vertices.end(), {{2*units,0,0},{3*units,0,0},{2*units,0,units}});
        mesh->Triangles.push_back({3,5,4});
        body.Shape = TriangleMeshShapeDesc{mesh};
        Require(physics.CreateBody(body).IsValid(), "Sliver cleanup removed valid supporting geometry");
        PhysicsRayHit hit;
        Require(physics.RayCastStatic({2.2f*units,units,0.2f*units},{0,-2*units,0},hit),
            "Sliver cleanup lost the supporting triangle");
        mesh = std::make_shared<PhysicsTriangleMesh>(*mesh);
        mesh->Vertices[2].z = 0.0001f*units;
        mesh->Vertices.insert(mesh->Vertices.end(), {{4*units,0,0},{5*units,0,0},{4*units,0,0.005f*units},
            {100*units,0,0},{101*units,0,0},{100*units,0,units}});
        mesh->Triangles.insert(mesh->Triangles.end(), {{6,8,7},{9,11,10}});
        body.Shape = TriangleMeshShapeDesc{mesh};
        Require(physics.CreateBody(body).IsValid(), "Large-bounds mesh sanitization failed");
        Require(!physics.RayCastStatic({0.5f*units,units,0.00002f*units},{0,-2*units,0},hit),
            "Mesh compression precision was ignored for a thin triangle");
        Require(physics.RayCastStatic({4.2f*units,units,0.001f*units},{0,-2*units,0},hit),
            "Mesh sanitization removed a resolvable thin triangle");
        mesh = std::make_shared<PhysicsTriangleMesh>(*mesh);
        mesh->Vertices[0].x = std::numeric_limits<float>::quiet_NaN();
        body.Shape = TriangleMeshShapeDesc{mesh};
        Require(!physics.CreateBody(body).IsValid(), "Mesh cooker accepted a nonfinite vertex");
    }
    std::cout << "Triangle sliver rejection, surviving support and centimeter/meter units passed\n";
}

static void CheckJobs(JobSystem& pool)
{
    // The backend owns Jolt registration, including its allocator.
    JoltPhysicsBackend runtime;
    runtime.Initialize(pool);
    std::set<std::thread::id> workers;
    std::mutex mutex;
    for (int repeat = 0; repeat < 64; ++repeat)
    {
        JoltJobSystem jobs(pool);
        auto* barrier = jobs.CreateBarrier();
        std::array<std::atomic<int>, 128> counts{};
        std::atomic<int> joined{0}, nested{0};
        auto last = jobs.CreateJob("join", JPH::Color::sBlue, [&] { ++joined; }, 128);
        barrier->AddJob(last);
        auto root = jobs.CreateJob("fan out", JPH::Color::sRed, [&]
        {
            for (size_t i = 0; i < counts.size(); ++i)
            {
                auto child = jobs.CreateJob("child", JPH::Color::sGreen, [&, i, last]
                {
                    ++counts[i];
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        workers.insert(std::this_thread::get_id());
                    }
                    auto grandchild = jobs.CreateJob("nested", JPH::Color::sGreen, [&] { ++nested; });
                    barrier->AddJob(grandchild);
                    last.RemoveDependency();
                });
                barrier->AddJob(child);
            }
        });
        barrier->AddJob(root);
        jobs.WaitForJobs(barrier);
        jobs.DestroyBarrier(barrier);
        Require(joined == 1 && nested == 128, "Jolt dependency/barrier work was lost or repeated");
        for (auto& count : counts) Require(count == 1, "Jolt child ran more or less than once");
    }
    std::cout << "Shared Jolt scheduler: " << pool.GetThreadCount() << " threads, observed "
        << workers.size() << " execution threads; nested fan-out/fan-in and repeated drain passed\n";
    if (pool.GetThreadCount() > 1) Require(workers.size() > 1, "Jolt never used shared workers");
}

static std::vector<Vector3> CheckBodies(JobSystem& jobs)
{
    JoltPhysicsBackend physics;
    physics.Initialize(jobs);
    PhysicsBodyDesc floor;
    floor.Shape = BoxShapeDesc{{50, 1, 50}};
    floor.Position = {0, -1, 0};
    const auto ground = physics.CreateBody(floor);
    Require(ground.IsValid(), "Floor creation failed");
    std::vector<PhysicsBodyHandle> bodies;
    for (unsigned i = 0; i < 1000; ++i)
    {
        PhysicsBodyDesc desc;
        desc.MotionType = PhysicsMotionType::Dynamic;
        desc.Shape = SphereShapeDesc{0.25f};
        desc.Position = {static_cast<float>(i % 40) - 20, 2.0f, static_cast<float>(i / 40) - 12};
        auto body = physics.CreateBody(desc);
        Require(body.IsValid(), "Stress body creation failed");
        bodies.push_back(body);
    }
    for (int tick = 0; tick < 180; ++tick) physics.Step(1.0f / 60);
    std::vector<Vector3> positions;
    for (auto body : bodies)
    {
        Vector3 position;
        Quaternion rotation;
        Require(physics.TryGetBodyTransform(body, position, rotation), "Live body missing");
        Require(std::isfinite(position.y) && std::abs(position.y - 0.25f) < 0.05f,
            "Dynamic body did not settle on the floor");
        positions.push_back(position);
        physics.DestroyBody(body);
        Require(!physics.TryGetBodyTransform(body, position, rotation), "Destroyed handle still resolves");
    }
    physics.DestroyBody(ground);
    auto invalid = floor;
    invalid.Scale.x = -1;
    Require(!physics.CreateBody(invalid).IsValid(), "Invalid collider dimensions accepted");
    return positions;
}

static void CheckWorlds(JobSystem& jobs)
{
    BenchmarkEngine engine;
    auto& locator = BenchmarkEngine::GetLocator();
    auto scenes = std::make_shared<SceneManagerModule>();
    auto physics = std::make_shared<PhysicsModule>(jobs);
    locator.RegisterModule(scenes);
    locator.RegisterModule(physics);
    physics->Initialize();
    World first(WorldDesc{}), second(WorldDesc{});
    scenes->OnWorldCreated.Broadcast(first);
    scenes->OnWorldCreated.Broadcast(second);
    const auto create = [](World& world, Vector3 position = Vector3(0, 10, 0))
    {
        auto entity = world.GetECS().CreateEntity();
        entity.AddComponent<RigidBodyComponent>().MotionType = PhysicsMotionType::Dynamic;
        entity.AddComponent<ColliderComponent>();
        // Transform last must also trigger body creation.
        entity.AddComponent<TransformComponent>(position);
        return entity.GetId();
    };
    auto a = create(first), b = create(second);
    std::vector<Entity> dynamicEntities{a};
    for (unsigned i = 1; i < 513; ++i)
        dynamicEntities.push_back(create(first, Vector3(300.f * (i % 32), 10, 300.f * (i / 32))));
    auto& ecs = first.GetECS();
    auto handle = ecs.Get<RigidBodyComponent>(a).Body;
    Require(handle.IsValid() && second.GetECS().Get<RigidBodyComponent>(b).Body.IsValid(), "Body subscription missed creation");
    second.SetPaused(true);
    for (int tick = 0; tick < 60; ++tick)
    {
        physics->Step(first, 1.0f / 60);
        physics->Step(second, 1.0f / 60);
    }
    DynamicBodyUpdateSystem sync(*physics);
    System& configuredSync = sync;
    configuredSync.Schedule(jobs, first, 0).Complete();
    sync.Schedule(jobs, second, 0).Complete();
    for (const auto entity : dynamicEntities)
    {
        Vector3 position;
        Quaternion rotation;
        Require(physics->TryGetBodyTransform(first, ecs.Get<RigidBodyComponent>(entity).Body, position, rotation) &&
            Vector3::Distance(position, ecs.Get<TransformComponent>(entity).Location) < 0.001f,
            "Scheduled dynamic-body synchronization missed a moving body across batch boundaries");
    }
    Require(ecs.Get<TransformComponent>(a).Location.y < 8, "Physics world did not advance");
    Require(second.GetECS().Get<TransformComponent>(b).Location.y == 10, "Paused world advanced with another world");
    const auto publishedRotation = ecs.Get<TransformComponent>(a).Rotation;
    ecs.Get<TransformComponent>(a).Rotation += Vector3(15, 30, 45);
    ecs.Get<TransformComponent>(a).DirtyFlag = false;
    sync.Schedule(jobs, first, 0).Complete();
    Require(ecs.Get<TransformComponent>(a).Rotation == publishedRotation && ecs.Get<TransformComponent>(a).DirtyFlag,
        "An unchanged physics quaternion must still overwrite manual ECS rotation edits");
    ecs.Remove<ColliderComponent>(a);
    Vector3 position;
    Quaternion rotation;
    Require(!physics->TryGetBodyTransform(first, handle, position, rotation), "Collider removal leaked physics body");
    ecs.Get<TransformComponent>(a).Rotation = Vector3(0, 37, 0);
    ecs.Add<ColliderComponent>(a);
    Require(ecs.Get<RigidBodyComponent>(a).Body.IsValid(), "Collider replacement missed body recreation");
    sync.Schedule(jobs, first, 0).Complete();
    Require(std::abs(ecs.Get<TransformComponent>(a).Rotation.y - 37) < 0.001f,
        "A recreated body's changed quaternion retained cached Euler angles");
    scenes->OnWorldDestroyed.Broadcast(first);
    Require(!ecs.Get<RigidBodyComponent>(a).Body.IsValid(), "World unload retained body handle");
    second.SetPaused(false);
    physics->Step(second, 1.0f / 60);
    scenes->OnWorldDestroyed.Broadcast(second);
    locator.UnregisterModule<PhysicsModule>();
    locator.UnregisterModule<SceneManagerModule>();
    std::cout << "World isolation, pause, subscriptions, collider replacement and unload passed\n";
}

static void CheckCompoundShapes(JobSystem& jobs)
{
    JoltPhysicsBackend physics;
    PhysicsWorldSettings settings;
    settings.WorldUnitsPerMeter = 100;
    physics.Initialize(jobs, settings);
    auto geometry = std::make_shared<PhysicsCompound>();
    geometry->Children = {
        {BoxShapeDesc{{50, 5, 20}}, {1000, 100, -400}, Quaternion::CreateFromAxisAngle(Vector3::UnitZ, XM_PIDIV2)},
        {CylinderShapeDesc{10, 20}, {1100, 100, -400}},
        {SphereShapeDesc{10}, {1200, 100, -400}},
        {CapsuleShapeDesc{10, 10}, {1300, 100, -400}}
    };
    auto hull = std::make_shared<PhysicsConvexCompound>();
    hull->Hulls.emplace_back();
    for (float x : {-10.f,10.f}) for (float y : {-10.f,10.f}) for (float z : {-10.f,10.f})
        hull->Hulls.front().push_back({x,y,z});
    geometry->Children.push_back({ConvexCompoundShapeDesc{hull}, {1400,100,-400}});
    PhysicsBodyDesc desc;
    desc.Shape = CompoundShapeDesc{geometry};
    const auto first = physics.CreateBody(desc);
    Require(first.IsValid(), "Primitive compound creation failed");
    PhysicsRayHit hit;
    for (const auto& sample : {Vector3(1000, 150, -400), Vector3(1100, 120, -400), Vector3(1108, 120, -400),
        Vector3(1200, 110, -400), Vector3(1300, 120, -400), Vector3(1400, 110, -400)})
    {
        Require(physics.RayCastStatic({sample.x, 300, sample.z}, {0, -400, 0}, hit) &&
            std::abs(hit.Position.y - sample.y) < 0.05f, "Compound local transforms or centimeter units are wrong");
    }
    Require(!physics.RayCastStatic({1050, 300, -400}, {0, -400, 0}, hit), "Compound filled the space between children");
    desc.Position = {3000, 0, 0};
    Require(physics.CreateBody(desc).IsValid(), "Reusing a cooked compound failed");
    desc.Position = {6000, 0, 0};
    desc.Scale = {2, 1, 1};
    Require(!physics.CreateBody(desc).IsValid(), "Compound silently approximated nonuniformly scaled round children");
    auto boxOnly = std::make_shared<PhysicsCompound>();
    boxOnly->Children.push_back(geometry->Children.front());
    desc.Shape = CompoundShapeDesc{boxOnly};
    Require(physics.CreateBody(desc).IsValid(), "Nonuniformly scaled compound failed");
    Require(physics.RayCastStatic({8000, 300, -400}, {0, -400, 0}, hit) &&
        std::abs(hit.Position.y - 150) < 0.05f, "Cooked compound cache ignored scale or rotated child axes");
    auto sheared = std::make_shared<PhysicsCompound>(*boxOnly);
    sheared->Children.front().Rotation = Quaternion::CreateFromAxisAngle(Vector3::UnitZ, XM_PIDIV4);
    auto unsupported = desc;
    unsupported.Shape = CompoundShapeDesc{sheared};
    Require(!physics.CreateBody(unsupported).IsValid(), "Compound silently approximated a sheared box");
    auto invalidGeometry = std::make_shared<PhysicsCompound>(*geometry);
    invalidGeometry->Children.front().Rotation = {0, 0, 0, 0};
    auto invalid = desc;
    invalid.Shape = CompoundShapeDesc{invalidGeometry};
    Require(!physics.CreateBody(invalid).IsValid(), "Compound accepted an invalid child rotation");
    invalid = desc;
    invalid.Scale.x = std::numeric_limits<float>::quiet_NaN();
    Require(!physics.CreateBody(invalid).IsValid(), "Cooked compound cache accepted a nonfinite scale");

    std::weak_ptr<const PhysicsCompound> retained = geometry;
    desc.Shape = BoxShapeDesc{};
    invalid.Shape = BoxShapeDesc{};
    geometry.reset();
    invalidGeometry.reset();
    Require(!retained.expired(), "Cooked compound cache did not retain geometry identity");
    physics.Uninitialize();
    Require(retained.expired(), "Uninitializing physics retained cached geometry");

    physics.Initialize(jobs, settings);
    PhysicsBodyDesc floor;
    floor.Shape = BoxShapeDesc{{2000, 10, 2000}};
    floor.Position = {0, -10, 0};
    Require(physics.CreateBody(floor).IsValid(), "Compound pivot test floor failed");
    auto baked = std::make_shared<PhysicsCompound>();
    const Vector3 center(1000, 0, -400);
    baked->Children.push_back({BoxShapeDesc{{20, 20, 10}}, center});
    desc.Shape = CompoundShapeDesc{baked};
    desc.Position = {0, 200, 0};
    desc.Scale = Vector3::One;
    desc.Rotation = Quaternion::CreateFromAxisAngle(Vector3::UnitY, XM_PIDIV4);
    desc.MotionType = PhysicsMotionType::Dynamic;
    const auto dynamic = physics.CreateBody(desc);
    Require(dynamic.IsValid(), "Baked primitive compound body failed");
    Vector3 position;
    Quaternion rotation;
    Require(physics.TryGetBodyTransform(dynamic, position, rotation) &&
        Vector3::Distance(position, desc.Position) < 0.05f, "Compound changed the baked body origin");
    for (int tick = 0; tick < 180; ++tick) physics.Step(1.f / 60);
    Require(physics.TryGetBodyTransform(dynamic, position, rotation), "Baked compound body disappeared");
    const auto restingCenter = Vector3::Transform(center, rotation) + position;
    Require(std::abs(restingCenter.y - 20) < 3, "Baked compound failed to settle on the real collision floor");
    std::cout << "Primitive compound offsets, rotations, units, scale, cache lifetime and baked pivots passed\n";
}

static void CheckPlacementQueries(JobSystem& jobs)
{
    for (float units : {1.f, 100.f})
    {
        JoltPhysicsBackend physics;
        PhysicsWorldSettings settings;
        settings.WorldUnitsPerMeter = units;
        physics.Initialize(jobs, settings);
        const Vector3 bakedCenter = Vector3(10, 0, -4) * units;
        const Vector3 legOffset = Vector3(2, 0, 0) * units;
        auto legs = std::make_shared<PhysicsCompound>();
        for (float sign : {-1.f, 1.f})
            legs->Children.push_back({BoxShapeDesc{Vector3(0.25f, 1, 0.25f) * units}, bakedCenter + sign * legOffset});
        PhysicsBodyDesc query;
        query.Shape = CompoundShapeDesc{legs};
        query.Position = Vector3(3, 5, -2) * units;
        query.Rotation = Quaternion::CreateFromYawPitchRoll(0.6f, 0.2f, -0.1f);
        query.Scale = {2, 1, 0.5f};
        const auto gap = Vector3::Transform(bakedCenter * query.Scale, query.Rotation) + query.Position;
        PhysicsBodyDesc obstacle;
        obstacle.Shape = BoxShapeDesc{Vector3(0.2f) * units};
        obstacle.Position = gap;
        const auto obstacleBody = physics.CreateBody(obstacle);
        Require(obstacleBody.IsValid(), "Placement query obstacle failed");
        Require(physics.OverlapsBox(gap, Vector3(6 * units)), "Placement AABB did not enclose the obstacle");
        Require(!physics.OverlapsBody(query), "Shape query filled the empty space between compound legs");
        auto shifted = query;
        shifted.Position -= Vector3::Transform(legOffset * query.Scale, query.Rotation);
        Require(physics.OverlapsBody(shifted), "Shape query lost rotated, scaled, baked-origin compound overlap");
        physics.DestroyBody(obstacleBody);
        Require(!physics.OverlapsBody(shifted), "Shape query retained a destroyed obstacle");
        obstacle.MotionType = PhysicsMotionType::Dynamic;
        const auto dynamicObstacle = physics.CreateBody(obstacle);
        Require(dynamicObstacle.IsValid() && physics.OverlapsBody(shifted), "Shape query ignored a dynamic obstacle");
        physics.DestroyBody(dynamicObstacle);
        const auto matchingBody = physics.CreateBody(query);
        Require(matchingBody.IsValid() && physics.OverlapsBody(query), "Shape query transform differs from body creation");
        physics.DestroyBody(matchingBody);

        obstacle = PhysicsBodyDesc{};
        obstacle.Shape = BoxShapeDesc{Vector3(units)};
        Require(physics.CreateBody(obstacle).IsValid(), "Touching query obstacle failed");
        PhysicsBodyDesc box;
        box.Shape = BoxShapeDesc{Vector3(units)};
        box.Position = {2 * units, 0, 0};
        Require(physics.OverlapsBody(box), "Shape query missed touching boxes");
        box.Position.x = 1.99f * units;
        Require(physics.OverlapsBody(box), "Shape query missed overlapping boxes");
        box.Position.x = 2.01f * units;
        Require(!physics.OverlapsBody(box), "Shape query reported separated boxes as overlapping");
        auto expectInvalid = [&](const PhysicsBodyDesc& invalid)
        {
            bool rejected = false;
            try { physics.OverlapsBody(invalid); }
            catch (const std::invalid_argument&) { rejected = true; }
            Require(rejected, "Invalid placement query silently returned a collision result");
        };
        auto invalid = box;
        invalid.Position.x = std::numeric_limits<float>::quiet_NaN();
        expectInvalid(invalid);
        invalid = box;
        invalid.Rotation = {0, 0, 0, 0};
        expectInvalid(invalid);
        invalid = box;
        invalid.Scale.x = std::numeric_limits<float>::infinity();
        expectInvalid(invalid);
        invalid = box;
        invalid.Shape = BoxShapeDesc{{-1, 1, 1}};
        expectInvalid(invalid);
        for (float radius : {-1.f, std::numeric_limits<float>::quiet_NaN()})
        {
            invalid.Shape=BoxShapeDesc{{1,1,1},radius};
            expectInvalid(invalid);
            auto hull=std::make_shared<PhysicsConvexCompound>();
            hull->Hulls={{{0,0,0},{1,0,0},{0,1,0},{0,0,1}}};
            hull->ConvexRadiusMeters=radius;
            invalid.Shape=ConvexCompoundShapeDesc{hull};
            expectInvalid(invalid);
        }
    }
    std::cout << "Shape-aware placement: compound gaps, baked pivots, transforms, contact, validation and units passed\n";
}

static void CheckStaticMeshCompound(JobSystem& jobs)
{
    for (float units : {1.f, 100.f})
    {
        JoltPhysicsBackend physics;
        PhysicsWorldSettings settings;
        settings.WorldUnitsPerMeter = units;
        physics.Initialize(jobs, settings);
        auto mesh = std::make_shared<PhysicsTriangleMesh>();
        for (const auto& point : {Vector3(-2, 0, -2), Vector3(2, 0, -2), Vector3(2, 0, 2), Vector3(-2, 0, 2)})
            mesh->Vertices.push_back(point * units);
        mesh->Triangles = {{0, 2, 1}, {0, 3, 2}};
        auto mixed = std::make_shared<PhysicsCompound>();
        mixed->Children = {
            {TriangleMeshShapeDesc{mesh}, Vector3(10, 0, -4) * units},
            {BoxShapeDesc{Vector3(0.5f) * units}, Vector3(14, 0.5f, -4) * units}
        };
        PhysicsBodyDesc desc;
        desc.Shape = CompoundShapeDesc{mixed};
        Require(physics.CreateBody(desc).IsValid(), "Static mixed mesh and box compound failed");
        PhysicsRayHit hit;
        Require(physics.RayCastStatic(Vector3(10, 3, -4) * units, Vector3(0, -5, 0) * units, hit) &&
            std::abs(hit.Position.y) < 0.001f * units, "Mixed compound mesh child transform is wrong");
        Require(physics.RayCastStatic(Vector3(14, 3, -4) * units, Vector3(0, -5, 0) * units, hit) &&
            std::abs(hit.Position.y - units) < 0.001f * units, "Mixed compound box child transform is wrong");
        for (auto motion : {PhysicsMotionType::Dynamic, PhysicsMotionType::Kinematic})
        {
            desc.MotionType = motion;
            bool rejected = false;
            try { physics.CreateBody(desc); }
            catch (const std::invalid_argument&) { rejected = true; }
            Require(rejected, "Moving body accepted a nested static-only mesh shape");
        }
        bool rejected = false;
        try { physics.OverlapsBody(desc); }
        catch (const std::invalid_argument&) { rejected = true; }
        Require(rejected, "Placement query accepted a static-only compound");
    }
    std::cout << "Static mixed mesh/box compound and recursive motion restrictions passed\n";
}

class PhysicsTestApp final : public App
{
public:
    PhysicsTestApp() : App(nullptr) {}
    using App::UpdatePhysics;
    void UseConfiguredSystems(bool enabled) { _AppConfig.StartScene = enabled ? "test" : ""; }
};

static void CheckFixedPipeline()
{
    PhysicsTestApp app;
    auto& locator = BenchmarkEngine::GetLocator();
    auto scenes = std::make_shared<SceneManagerModule>();
    auto physics = std::make_shared<PhysicsModule>(app.GetJobs());
    locator.RegisterModule(scenes);
    locator.RegisterModule(physics);
    physics->Initialize();
    const auto failedWorld = scenes->OnWorldCreated.AddListener([](World& world)
    {
        auto entity = world.GetECS().CreateEntity();
        entity.AddComponent<RigidBodyComponent>();
        entity.AddComponent<ColliderComponent>();
        entity.AddComponent<TransformComponent>();
        throw std::runtime_error("Injected load cancellation");
    });
    unsigned cancelled = 0;
    const auto destroyed = scenes->OnWorldDestroyed.AddListener([&](World& world)
    {
        ++cancelled;
        world.GetECS().ForEach<RigidBodyComponent>([](Entity, RigidBodyComponent& body)
        { Require(!body.Body.IsValid(), "Cancelled world retained physics bodies"); });
    });
    const auto fixture = std::filesystem::temp_directory_path() /
        ("pepe-jolt-" + std::to_string(GetCurrentProcessId()) + ".world");
    auto sceneFixture = fixture;
    sceneFixture.replace_extension(".yaml");
    std::ofstream(fixture) << "World:\n  Name: CancellationTest\n";
    std::ofstream(sceneFixture) << "Scene:\n  Name: CancellationTest\n  Worlds:\n    - Path: '"
        << fixture.generic_string() << "'\n";
    for (bool configured : {false, true})
    {
        bool threw = false;
        try
        {
            if (configured) scenes->LoadScene(sceneFixture.string(), AppConfig{});
            else scenes->LoadWorld(fixture);
        }
        catch (const std::runtime_error&) { threw = true; }
        Require(threw && scenes->GetWorldCount() == 0, "Cancelled load was retained");
    }
    Require(cancelled == 2, "Load cancellation missed world cleanup");
    scenes->OnWorldCreated.RemoveListener(failedWorld);
    scenes->OnWorldDestroyed.RemoveListener(destroyed);
    std::filesystem::remove(fixture);
    std::filesystem::remove(sceneFixture);
    constexpr float step = 1.0f / 60;
    for (bool configured : {false, true})
    {
        app.UseConfiguredSystems(configured);
        World world(WorldDesc{});
        scenes->OnWorldCreated.Broadcast(world);
        auto entity = world.GetECS().CreateEntity();
        entity.AddComponent<RigidBodyComponent>().MotionType = PhysicsMotionType::Kinematic;
        entity.AddComponent<ColliderComponent>();
        entity.AddComponent<TransformComponent>();
        entity.AddComponent<VelocityComponent>().Velocity = {60, 0, 0};
        if (configured)
        {
            scenes->AddSystem<MovementSystem>(&world, 0, SystemUpdatePhase::FixedPrePhysics);
            scenes->AddSystem<KinematicBodyUpdateSystem>(&world, 1, *physics);
            scenes->AddSystem<DynamicBodyUpdateSystem>(&world, 2, *physics);
        }
        const auto check = [&](float expected)
        {
            Vector3 position;
            Quaternion rotation;
            Require(physics->TryGetBodyTransform(world, entity.GetComponent<RigidBodyComponent>().Body,
                position, rotation), "Fixed-step body missing");
            Require(std::abs(position.x - expected) < 0.002f, "Fixed-step accumulation, scale or phase order failed");
        };
        app.UpdatePhysics(world, step / 2); check(0);
        app.UpdatePhysics(world, step / 2); check(1);
        world.SetPaused(true);
        app.UpdatePhysics(world, step * 10); check(1);
        world.SetPaused(false);
        world.SetTimeScale(2);
        app.UpdatePhysics(world, step / 2); check(2);
        world.SetTimeScale(1);
        app.UpdatePhysics(world, step * 9); check(10);
        app.UpdatePhysics(world, step * 1.01f); check(12);
        world.GetECS().Remove<RigidBodyComponent>(entity.GetId());
        auto curveEntity = world.GetECS().CreateEntity();
        auto& curve = curveEntity.AddComponent<SplineCurveComponent>();
        curve.Points.resize(1);
        curve.Points[0].Position = {17, 0, 0};
        entity.AddComponent<SplineFollowComponent>(curveEntity.GetId(), 1.0f, false, true, 0.0f, SplineUpdateMode::Fixed);
        if (configured) scenes->AddSystem<SplineFollowSystem>(&world, 0, SplineUpdateMode::Fixed);
        app.UpdatePhysics(world, step);
        Require(entity.GetComponent<TransformComponent>().Location.x == 17,
            "Removing the last rigid body stopped independent fixed systems");
        World overloaded(WorldDesc{});
        scenes->OnWorldCreated.Broadcast(overloaded);
        unsigned slowTicks = 0;
        overloaded.BeforePhysicsStep = [&](float)
        {
            if (++slowTicks == 1) Sleep(25);
        };
        app.UpdatePhysics(overloaded, step * 8.01f);
        Require(slowTicks == 1, "Catch-up repeated an expensive physics tick past the frame budget");
        overloaded.BeforePhysicsStep = [&](float) { ++slowTicks; };
        for (unsigned frame = 0; frame < 16 && slowTicks < 8; ++frame)
            app.UpdatePhysics(overloaded, step * 0.0001f);
        Require(slowTicks == 8, "Physics frame budget discarded the accumulated simulation time");
        scenes->OnWorldDestroyed.Broadcast(overloaded);
        scenes->OnWorldDestroyed.Broadcast(world);
        scenes->Uninitialize();
    }
    std::cout << "Load cancellation, default/configured fixed phases, pause, time scale and retained catch-up passed\n";
}

int main()
{
    try
    {
        std::vector<Vector3> baseline;
        for (unsigned threads : {1u, 4u})
        {
            JobSystem jobs(threads);
#ifdef _DEBUG
            CheckCollisionGeometry(jobs);
#endif
            CheckTriangleSanitization(jobs);
            CheckJobs(jobs);
            auto positions = CheckBodies(jobs);
            if (baseline.empty()) baseline = positions;
            else for (size_t i = 0; i < positions.size(); ++i)
                Require(Vector3::Distance(baseline[i], positions[i]) < 0.001f, "Physics result depends on worker count");
            CheckWorlds(jobs);
            CheckCompoundShapes(jobs);
            CheckPlacementQueries(jobs);
            CheckStaticMeshCompound(jobs);
        }
        CheckFixedPipeline();
        std::cout << "Jolt integration passed: 1000 colliding bodies, 180 ticks, 1/4 shared threads\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
