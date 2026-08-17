#include "App.Base/Modules/PhysicsModule.h"
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
