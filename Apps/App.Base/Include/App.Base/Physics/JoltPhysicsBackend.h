#pragma once

#include <Jolt/Jolt.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Core/JobSystemThreadPool.h>
class JoltPhysicsBackend
{
public:
    void Initialize();
    void Uninitialize();

    void Step(float dt);

private:
    JPH::PhysicsSystem _physicsSystem;
    
    std::unique_ptr<JPH::TempAllocator> _tempAllocator;
    std::unique_ptr<JPH::JobSystemThreadPool> _jobSystem; // tmp
    
    JPH::BodyID _floorBody;
    JPH::BodyID _boxBody;
};



