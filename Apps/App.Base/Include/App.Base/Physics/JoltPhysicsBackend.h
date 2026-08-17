#pragma once

#include <memory>

namespace JPH
{
    class PhysicsSystem;
    class TempAllocatorImpl;
    class JobSystemThreadPool;
}

class BroadPhaseLayerInterfaceImpl;
class ObjectVsBroadPhaseLayerFilterImpl;
class ObjectLayerPairFilterImpl;

class JoltPhysicsBackend
{
public:
    JoltPhysicsBackend();
    ~JoltPhysicsBackend();
    
    void Initialize();
    void Uninitialize();

    void Step(float dt);

private:
    bool _bInitialized = false;
    
    std::unique_ptr<JPH::PhysicsSystem> _physicsSystem;
    
    std::unique_ptr<JPH::TempAllocatorImpl> _tempAllocator;
    std::unique_ptr<JPH::JobSystemThreadPool> _jobSystem; // tmp
    
    std::unique_ptr<BroadPhaseLayerInterfaceImpl> _broadPhaseLayerInterface;
    std::unique_ptr<ObjectVsBroadPhaseLayerFilterImpl> _objectVsBroadPhaseLayerFilter;
    std::unique_ptr<ObjectLayerPairFilterImpl> _objectLayerPairFilter;
};



