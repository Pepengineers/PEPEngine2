#include "App.Base/Modules/PhysicsModule.h"

#include "App.Base/ECS/World.h"
#include "App.Base/Physics/JoltPhysicsBackend.h"

#include "App.Base/Modules/SceneManagerModule.h"
#include "Engine.Core/BenchmarkEngine.h"
#include "Common/Logger.h"

PhysicsModule::PhysicsModule(GameTimer* gt) : _gt(gt)
{
}

PhysicsModule::~PhysicsModule()
{
}

void PhysicsModule::Initialize()
{
    _cpuPhysics = std::make_unique<JoltPhysicsBackend>();
    _cpuPhysics->Initialize();
    
    SubscribeToSceneManager();
}

void PhysicsModule::Uninitialize()
{
    UnsubscribeFromSceneManager();
    
    if (_cpuPhysics)
    {
        _cpuPhysics->Uninitialize();
        _cpuPhysics.reset();
    }
}

void PhysicsModule::Step(float dt)
{
    if (!_cpuPhysics) { return; }
    _cpuPhysics->Step(dt);
}

PhysicsBodyHandle PhysicsModule::CreateBoxBody(const PhysicsBoxBodyDesc& desc)
{
    if (!_cpuPhysics) { return {}; }
    return _cpuPhysics->CreateBoxBody(desc);
}

void PhysicsModule::DestroyBody(PhysicsBodyHandle handle)
{
    if (!_cpuPhysics || !handle.IsValid()) { return; }
    _cpuPhysics->DestroyBody(handle);
}

bool PhysicsModule::TryGetBodyTransform(PhysicsBodyHandle handle, DirectX::SimpleMath::Vector3& outPosition,
    DirectX::SimpleMath::Quaternion& outRotation) const
{
    if (!_cpuPhysics) { return false; }
    return _cpuPhysics->TryGetBodyTransform(handle, outPosition, outRotation);
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

// TODO make generalized, not only for box bodies
void PhysicsModule::TryCreateBody(World& world, Entity entity)
{
    auto& ecs = world.GetECS();
    
    if (!ecs.Has<RigidBodyComponent>(entity) || !ecs.Has<BoxColliderComponent>(entity) || !ecs.Has<TransformComponent>(entity)) { return; }
    
    auto& rigidBody = ecs.Get<RigidBodyComponent>(entity);
    auto& transform = ecs.Get<TransformComponent>(entity);
    auto& collider = ecs.Get<BoxColliderComponent>(entity);

    if (rigidBody.Body.IsValid()) { return; }
    
    PhysicsBoxBodyDesc desc;
    desc.Position = transform.Location;
    // TODO make quats everywhere in engine
    desc.Rotation = DirectX::SimpleMath::Quaternion::CreateFromYawPitchRoll(
        DirectX::XMConvertToRadians(transform.Rotation.y),
        DirectX::XMConvertToRadians(transform.Rotation.x),
        DirectX::XMConvertToRadians(transform.Rotation.z));
    desc.HalfExtent = DirectX::SimpleMath::Vector3(
        collider.HalfExtent.x * transform.Scale.x,
        collider.HalfExtent.y * transform.Scale.y,
        collider.HalfExtent.z * transform.Scale.z);
    desc.MotionType = rigidBody.MotionType;
    
    rigidBody.Body = CreateBoxBody(desc);
    
    // TODO remove later
    if (rigidBody.Body.IsValid())   
    {
        Logger::Info("Created physics body: entity={}, handle={}", entity, rigidBody.Body.Value);
    }
    else
    {
        Logger::Error("Failed to create physics body for entity={}", entity);
    }
}

void PhysicsModule::TryDestroyBody(World& world, Entity entity)
{
    auto& ecs = world.GetECS();
    if (!ecs.Has<RigidBodyComponent>(entity)) { return; }
    auto& rigidBody = ecs.Get<RigidBodyComponent>(entity);
    if (!rigidBody.Body.IsValid()) { return; }
    DestroyBody(rigidBody.Body);
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
    auto& colliderPool = ecs.GetPool<BoxColliderComponent>();
    
    WorldPhysicsSubscriptions subscriptions;
    
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
    
    _worldSubscriptions.emplace(&world, subscriptions);
    
    // TODO tmp
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
    auto& colliderPool = ecs.GetPool<BoxColliderComponent>();
    
    rigidBodyPool.OnComponentCreated.RemoveListener(subscriptions.RigidBodyCreated);
    rigidBodyPool.OnComponentDestroyed.RemoveListener(subscriptions.RigidBodyDestroyed);
    
    colliderPool.OnComponentCreated.RemoveListener(subscriptions.ColliderCreated);
    colliderPool.OnComponentDestroyed.RemoveListener(subscriptions.ColliderDestroyed);
    
    _worldSubscriptions.erase(it);
}
