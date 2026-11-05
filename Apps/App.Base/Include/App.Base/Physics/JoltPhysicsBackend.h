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

    void Initialize(JobSystem& jobs, const PhysicsWorldSettings& settings = {});
    bool OverlapsBox(const DirectX::SimpleMath::Vector3& center, const DirectX::SimpleMath::Vector3& halfExtent) const;
    bool RayCastStatic(const DirectX::SimpleMath::Vector3& origin, const DirectX::SimpleMath::Vector3& displacement, PhysicsRayHit& hit) const;
    void Uninitialize();

    void Step(float dt);

    PhysicsBodyHandle CreateBody(const PhysicsBodyDesc& desc);
    void DestroyBody(PhysicsBodyHandle handle);

    bool TryGetBodyTransform(PhysicsBodyHandle handle, DirectX::SimpleMath::Vector3& outPosition, DirectX::SimpleMath::Quaternion& outRotation) const;
    void MoveKinematicBody(PhysicsBodyHandle handle, const DirectX::SimpleMath::Vector3& targetPosition, const DirectX::SimpleMath::Quaternion& targetRotation, float dt);
    // World units/s; mass is applied internally. Wakes dynamics and sets a persistent speed cap.
    bool AddBodyVelocityImpulse(PhysicsBodyHandle handle, const DirectX::SimpleMath::Vector3& deltaVelocity, float maxSpeed);
    bool TryGetBodyVelocity(PhysicsBodyHandle handle, DirectX::SimpleMath::Vector3& velocity, bool& active) const;

private:
    bool _bInitialized = false;
    PhysicsWorldSettings _settings;

    std::unique_ptr<JPH::PhysicsSystem> _physicsSystem;

    std::unique_ptr<JPH::TempAllocatorImpl> _tempAllocator;
    std::unique_ptr<JoltJobSystem> _jobSystem;
    std::shared_ptr<JoltRuntime> _runtime;

    std::unique_ptr<BroadPhaseLayerInterfaceImpl> _broadPhaseLayerInterface;
    std::unique_ptr<ObjectVsBroadPhaseLayerFilterImpl> _objectVsBroadPhaseLayerFilter;
    std::unique_ptr<ObjectLayerPairFilterImpl> _objectLayerPairFilter;
};
