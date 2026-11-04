#pragma once

#include <memory>


#include <App.Base/Physics/PhysicsTypes.h>

namespace JPH
{
    class PhysicsSystem;
    class TempAllocatorImpl;
}

class JobSystem;
class JoltJobSystem;
struct JoltRuntime;

class BroadPhaseLayerInterfaceImpl;
class ObjectVsBroadPhaseLayerFilterImpl;
class ObjectLayerPairFilterImpl;

class JoltPhysicsBackend
{
public:
    JoltPhysicsBackend();
    ~JoltPhysicsBackend();

    void Initialize(JobSystem& jobs);
    void Uninitialize();

    void Step(float dt);

    PhysicsBodyHandle CreateBody(const PhysicsBodyDesc& desc);
    void DestroyBody(PhysicsBodyHandle handle);

    bool TryGetBodyTransform(PhysicsBodyHandle handle, DirectX::SimpleMath::Vector3& outPosition, DirectX::SimpleMath::Quaternion& outRotation) const;
    void MoveKinematicBody(PhysicsBodyHandle handle, const DirectX::SimpleMath::Vector3& targetPosition, const DirectX::SimpleMath::Quaternion& targetRotation, float dt);

private:
    bool _bInitialized = false;

    std::unique_ptr<JPH::PhysicsSystem> _physicsSystem;

    std::unique_ptr<JPH::TempAllocatorImpl> _tempAllocator;
    std::unique_ptr<JoltJobSystem> _jobSystem;
    std::shared_ptr<JoltRuntime> _runtime;

    std::unique_ptr<BroadPhaseLayerInterfaceImpl> _broadPhaseLayerInterface;
    std::unique_ptr<ObjectVsBroadPhaseLayerFilterImpl> _objectVsBroadPhaseLayerFilter;
    std::unique_ptr<ObjectLayerPairFilterImpl> _objectLayerPairFilter;
};
