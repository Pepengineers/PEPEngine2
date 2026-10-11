#pragma once

#include <memory>

// test
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyID.h>

#include <App.Base/Physics/PhysicsTypes.h>

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
    
    // create bodies
    PhysicsBodyHandle CreateBoxBody(const PhysicsBoxBodyDesc& desc);
    
    // destroy bodies
    void DestroyBody(PhysicsBodyHandle handle);
    
    bool TryGetBodyTransform(PhysicsBodyHandle handle, DirectX::SimpleMath::Vector3& outPosition, DirectX::SimpleMath::Quaternion& outRotation) const;

private:
    bool _bInitialized = false;
    
    std::unique_ptr<JPH::PhysicsSystem> _physicsSystem;
    
    std::unique_ptr<JPH::TempAllocatorImpl> _tempAllocator;
    std::unique_ptr<JPH::JobSystemThreadPool> _jobSystem; // tmp
    
    std::unique_ptr<BroadPhaseLayerInterfaceImpl> _broadPhaseLayerInterface;
    std::unique_ptr<ObjectVsBroadPhaseLayerFilterImpl> _objectVsBroadPhaseLayerFilter;
    std::unique_ptr<ObjectLayerPairFilterImpl> _objectLayerPairFilter;
};



