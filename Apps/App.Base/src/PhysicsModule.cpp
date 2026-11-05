#include "App.Base/Modules/PhysicsModule.h"

#include "App.Base/ECS/World.h"
#include "App.Base/Physics/JoltPhysicsBackend.h"

#include "App.Base/Modules/SceneManagerModule.h"
#include "Engine.Core/BenchmarkEngine.h"
#include "Common/Logger.h"

PhysicsModule::PhysicsModule(JobSystem& jobs) : _jobs(jobs)
{
}

PhysicsModule::~PhysicsModule() = default;

void PhysicsModule::Initialize()
{
    SubscribeToSceneManager();
}

void PhysicsModule::Uninitialize()
{
    UnsubscribeFromSceneManager();

}

JoltPhysicsBackend* PhysicsModule::GetPhysics(World& world) const
{
    const auto it = _worldSubscriptions.find(&world);
    return it == _worldSubscriptions.end() ? nullptr : it->second.Physics.get();
}

void PhysicsModule::ConfigureWorld(World& world, const PhysicsWorldSettings& settings)
{
    if (world.GetECS().GetPool<RigidBodyComponent>().Size() != 0)
        throw std::logic_error("Configure physics before creating bodies");
    auto* physics = GetPhysics(world);
    if (!physics) throw std::logic_error("World has no physics backend");
    physics->Uninitialize();
    physics->Initialize(_jobs, settings);
}

bool PhysicsModule::OverlapsBox(World& world, const Vector3& center, const Vector3& halfExtent) const
{
    auto* physics = GetPhysics(world);
    if (!physics) throw std::logic_error("World has no physics backend");
    return physics->OverlapsBox(center, halfExtent);
}

bool PhysicsModule::RayCastStatic(World& world, const Vector3& origin, const Vector3& displacement, PhysicsRayHit& hit) const
{
    auto* physics = GetPhysics(world);
    if (!physics) throw std::logic_error("World has no physics backend");
    return physics->RayCastStatic(origin,displacement,hit);
}

void PhysicsModule::Step(World& world, float dt)
{
    if (auto* physics = GetPhysics(world); physics && !world.IsPaused()) physics->Step(dt);
}

PhysicsBodyHandle PhysicsModule::CreateBody(World& world, const PhysicsBodyDesc& desc)
{
    auto* physics = GetPhysics(world);
    return physics ? physics->CreateBody(desc) : PhysicsBodyHandle{};
}

void PhysicsModule::DestroyBody(World& world, PhysicsBodyHandle handle)
{
    if (auto* physics = GetPhysics(world); physics && handle.IsValid()) physics->DestroyBody(handle);
}

bool PhysicsModule::AddBodyVelocityImpulse(World& world, PhysicsBodyHandle handle, const Vector3& deltaVelocity, float maxSpeed)
{
    auto* physics = GetPhysics(world);
    return physics && physics->AddBodyVelocityImpulse(handle, deltaVelocity, maxSpeed);
}

bool PhysicsModule::TryGetBodyVelocity(World& world, PhysicsBodyHandle handle, Vector3& velocity, bool& active) const
{
    auto* physics = GetPhysics(world);
    return physics && physics->TryGetBodyVelocity(handle, velocity, active);
}

bool PhysicsModule::TryGetBodyTransform(World& world, PhysicsBodyHandle handle,
    DirectX::SimpleMath::Vector3& position, DirectX::SimpleMath::Quaternion& rotation) const
{
    auto* physics = GetPhysics(world);
    return physics && physics->TryGetBodyTransform(handle, position, rotation);
}

void PhysicsModule::MoveKinematicBody(World& world, PhysicsBodyHandle handle,
    const DirectX::SimpleMath::Vector3& position, const DirectX::SimpleMath::Quaternion& rotation, float dt)
{
    if (auto* physics = GetPhysics(world)) physics->MoveKinematicBody(handle, position, rotation, dt);
}

void PhysicsModule::OnUpdate()
{
}

void PhysicsModule::OnRender()
{
}

bool PhysicsModule::ShouldTick()
{
    return false;
}

bool PhysicsModule::ShouldRender()
{
    return false;
}

void PhysicsModule::TryCreateBody(World& world, Entity entity)
{
    auto& ecs = world.GetECS();

    if (!ecs.Has<RigidBodyComponent>(entity) || !ecs.Has<ColliderComponent>(entity) || !ecs.Has<TransformComponent>(entity)) { return; }

    auto& rigidBody = ecs.Get<RigidBodyComponent>(entity);
    auto& transform = ecs.Get<TransformComponent>(entity);
    auto& collider = ecs.Get<ColliderComponent>(entity);

    if (rigidBody.Body.IsValid()) { return; }

    PhysicsBodyDesc desc;
    desc.Position = transform.Location;
    desc.Rotation = DirectX::SimpleMath::Quaternion::CreateFromYawPitchRoll(
        DirectX::XMConvertToRadians(transform.Rotation.y),
        DirectX::XMConvertToRadians(transform.Rotation.x),
        DirectX::XMConvertToRadians(transform.Rotation.z));
    desc.Shape = collider.Shape;
    desc.Scale = transform.Scale;
    desc.MotionType = rigidBody.MotionType;
    desc.ContinuousCollision = rigidBody.ContinuousCollision;

    rigidBody.Body = CreateBody(world, desc);

    if (!rigidBody.Body.IsValid())
    {
        throw std::runtime_error("Failed to create physics body for entity=" + std::to_string(entity));
    }
}

void PhysicsModule::TryDestroyBody(World& world, Entity entity)
{
    auto& ecs = world.GetECS();
    if (!ecs.Has<RigidBodyComponent>(entity)) { return; }
    auto& rigidBody = ecs.Get<RigidBodyComponent>(entity);
    if (!rigidBody.Body.IsValid()) { return; }
    DestroyBody(world, rigidBody.Body);
    rigidBody.Body = {};
}

void PhysicsModule::SubscribeToSceneManager()
{
    auto sceneManager = BenchmarkEngine::GetLocator().GetModule<SceneManagerModule>();
    if (!sceneManager) { return; }

    if (_worldCreatedListener != 0 || _worldDestroyedListener != 0) { return; }

    _worldCreatedListener = sceneManager->OnWorldCreated.AddListener([this](World& world)
    {
       SubscribeToWorld(world);
    });

    _worldDestroyedListener = sceneManager->OnWorldDestroyed.AddListener([this](World& world)
    {
       UnsubscribeFromWorld(world);
    });

    for (size_t i = 0; i < sceneManager->GetWorldCount(); ++i)
    {
        if (World* world = sceneManager->GetWorld(i))
        {
            SubscribeToWorld(*world);
        }
    }
}

void PhysicsModule::UnsubscribeFromSceneManager()
{
    auto sceneManager = BenchmarkEngine::GetLocator().GetModule<SceneManagerModule>();

    if (sceneManager)
    {
        if (_worldCreatedListener != 0)
        {
            sceneManager->OnWorldCreated.RemoveListener(_worldCreatedListener);
        }

        if (_worldDestroyedListener != 0)
        {
            sceneManager->OnWorldDestroyed.RemoveListener(_worldDestroyedListener);
        }

        for (size_t i = 0; i < sceneManager->GetWorldCount(); ++i)
        {
            if (World* world = sceneManager->GetWorld(i))
            {
                UnsubscribeFromWorld(*world);
            }
        }
    }

    _worldCreatedListener = 0;
    _worldDestroyedListener = 0;
    _worldSubscriptions.clear();
}

void PhysicsModule::SubscribeToWorld(World& world)
{
    if (_worldSubscriptions.find(&world) != _worldSubscriptions.end()) { return; }

    auto& ecs = world.GetECS();

    auto& rigidBodyPool = ecs.GetPool<RigidBodyComponent>();
    auto& colliderPool = ecs.GetPool<ColliderComponent>();

    WorldPhysicsSubscriptions subscriptions;
    subscriptions.Physics = std::make_unique<JoltPhysicsBackend>();
    subscriptions.Physics->Initialize(_jobs);

    auto onCreated = [this, &world](Entity entity, auto&)
    {
        TryCreateBody(world, entity);
    };

    auto onDestroyed = [this, &world](Entity entity, auto&)
    {
        TryDestroyBody(world, entity);
    };

    subscriptions.RigidBodyCreated = rigidBodyPool.OnComponentCreated.AddListener(onCreated);
    subscriptions.RigidBodyDestroyed = rigidBodyPool.OnComponentDestroyed.AddListener(onDestroyed);

    subscriptions.ColliderCreated = colliderPool.OnComponentCreated.AddListener(onCreated);
    subscriptions.ColliderDestroyed = colliderPool.OnComponentDestroyed.AddListener(onDestroyed);

    auto& transforms = ecs.GetPool<TransformComponent>();
    subscriptions.TransformCreated = transforms.OnComponentCreated.AddListener(onCreated);
    subscriptions.TransformDestroyed = transforms.OnComponentDestroyed.AddListener(onDestroyed);
    _worldSubscriptions.emplace(&world, std::move(subscriptions));

    ecs.ForEach<RigidBodyComponent>([this, &world](Entity entity, RigidBodyComponent&)
    {
       TryCreateBody(world, entity);
    });
}

void PhysicsModule::UnsubscribeFromWorld(World& world)
{
    auto it = _worldSubscriptions.find(&world);
    if (it == _worldSubscriptions.end()) { return; }

    auto& ecs = world.GetECS();

    ecs.ForEach<RigidBodyComponent>([this, &world](Entity entity, RigidBodyComponent&)
    {
        TryDestroyBody(world, entity);
    });

    const auto& subscriptions = it->second;

    auto& rigidBodyPool = ecs.GetPool<RigidBodyComponent>();
    auto& colliderPool = ecs.GetPool<ColliderComponent>();

    rigidBodyPool.OnComponentCreated.RemoveListener(subscriptions.RigidBodyCreated);
    rigidBodyPool.OnComponentDestroyed.RemoveListener(subscriptions.RigidBodyDestroyed);

    colliderPool.OnComponentCreated.RemoveListener(subscriptions.ColliderCreated);
    colliderPool.OnComponentDestroyed.RemoveListener(subscriptions.ColliderDestroyed);

    auto& transforms = ecs.GetPool<TransformComponent>();
    transforms.OnComponentCreated.RemoveListener(subscriptions.TransformCreated);
    transforms.OnComponentDestroyed.RemoveListener(subscriptions.TransformDestroyed);
    _worldSubscriptions.erase(it);
}
