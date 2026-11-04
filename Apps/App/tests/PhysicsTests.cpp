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
        }
        CheckFixedPipeline();
        std::cout << "Jolt integration passed: 1000 colliding bodies, 180 ticks, 1/4 shared threads\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
