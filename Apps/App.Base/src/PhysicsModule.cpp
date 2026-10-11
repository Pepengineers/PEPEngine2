#include "App.Base/Modules/PhysicsModule.h"

#include "App.Base/ECS/World.h"
#include "App.Base/Physics/JoltPhysicsBackend.h"

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
}

void PhysicsModule::Uninitialize()
{
    if (_cpuPhysics)
    {
        _cpuPhysics->Uninitialize();
        _cpuPhysics.reset();
    }
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

void PhysicsModule::OnUpdate()
{
    Tick(_gt->DeltaTime());
}

void PhysicsModule::OnRender()
{
}

bool PhysicsModule::ShouldTick()
{
    return true;
}

bool PhysicsModule::ShouldRender()
{
    return false;
}

void PhysicsModule::Tick(float dt)
{
    _accumulatedTime += dt;

    while (_accumulatedTime >= FixedDeltaTime)
    {
        _cpuPhysics->Step(FixedDeltaTime);
        _accumulatedTime -= FixedDeltaTime;
    }
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
