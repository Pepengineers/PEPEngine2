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
    const auto create = [](World& world)
    {
        auto entity = world.GetECS().CreateEntity();
        entity.AddComponent<RigidBodyComponent>().MotionType = PhysicsMotionType::Dynamic;
        entity.AddComponent<ColliderComponent>();
        // Transform last must also trigger body creation.
        entity.AddComponent<TransformComponent>(Vector3(0, 10, 0));
        return entity.GetId();
    };
    auto a = create(first), b = create(second);
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
    sync.Schedule(jobs, first, 0).Complete();
    sync.Schedule(jobs, second, 0).Complete();
    Require(ecs.Get<TransformComponent>(a).Location.y < 8, "Physics world did not advance");
    Require(second.GetECS().Get<TransformComponent>(b).Location.y == 10, "Paused world advanced with another world");
    ecs.Remove<ColliderComponent>(a);
    Vector3 position;
    Quaternion rotation;
    Require(!physics->TryGetBodyTransform(first, handle, position, rotation), "Collider removal leaked physics body");
    ecs.Add<ColliderComponent>(a);
    Require(ecs.Get<RigidBodyComponent>(a).Body.IsValid(), "Collider replacement missed body recreation");
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
            CheckJobs(jobs);
            auto positions = CheckBodies(jobs);
            if (baseline.empty()) baseline = positions;
            else for (size_t i = 0; i < positions.size(); ++i)
                Require(Vector3::Distance(baseline[i], positions[i]) < 0.001f, "Physics result depends on worker count");
            CheckWorlds(jobs);
            CheckCompoundShapes(jobs);
        }
        CheckFixedPipeline();
        std::cout << "Jolt integration passed: 1000 colliding bodies, 180 ticks, 1/4 shared threads\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
