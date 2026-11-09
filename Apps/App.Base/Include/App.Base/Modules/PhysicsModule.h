#pragma once


#include <memory>

#include "Common/GameTimer.h"
#include "Common/Module.h"
#include "Engine.Core/ECS/Entity.h"
#include "App.Base/Physics/PhysicsTypes.h"

#include <unordered_map>
#include "Engine.Core/ECS/Event.h"

class World;
class SceneManagerModule;

class JoltPhysicsBackend;
class JobSystem;

class PhysicsModule final : public Module
{
public:
    explicit PhysicsModule(JobSystem& jobs);
    ~PhysicsModule() override;

    void Initialize() override;
    void Uninitialize() override;

    void Step(World& world, float dt);
#ifdef _DEBUG
    void CollectCollisionTriangles(World& world, std::vector<DirectX::SimpleMath::Vector3>& statics,
        std::vector<DirectX::SimpleMath::Vector3>& moving) const;
#endif
    void ConfigureWorld(World& world, const PhysicsWorldSettings& settings);
    bool OverlapsBox(World& world, const DirectX::SimpleMath::Vector3& center, const DirectX::SimpleMath::Vector3& halfExtent) const;
    bool OverlapsBody(World& world, const PhysicsBodyDesc& desc);
    bool RayCastStatic(World& world, const DirectX::SimpleMath::Vector3& origin, const DirectX::SimpleMath::Vector3& displacement, PhysicsRayHit& hit) const;

    PhysicsBodyHandle CreateBody(World& world, const PhysicsBodyDesc& desc);
    void DestroyBody(World& world, PhysicsBodyHandle handle);
    bool AddBodyVelocityImpulse(World& world, PhysicsBodyHandle handle, const DirectX::SimpleMath::Vector3& deltaVelocity, float maxSpeed);
    bool TryGetBodyVelocity(World& world, PhysicsBodyHandle handle, DirectX::SimpleMath::Vector3& velocity, bool& active) const;

    bool TryGetBodyTransform(World& world, PhysicsBodyHandle handle, DirectX::SimpleMath::Vector3& outPosition, DirectX::SimpleMath::Quaternion& outRotation) const;
    void MoveKinematicBody(World& world, PhysicsBodyHandle handle, const DirectX::SimpleMath::Vector3& targetPosition, const DirectX::SimpleMath::Quaternion& targetRotation, float dt);
protected:
    void OnUpdate() override;
    void OnRender() override;

    bool ShouldTick() override;
    bool ShouldRender() override;

private:
    void TryCreateBody(World& world, Entity entity);
    void TryDestroyBody(World& world, Entity entity);

    struct WorldPhysicsSubscriptions
    {
        std::unique_ptr<JoltPhysicsBackend> Physics;
        ListenerHandle TransformCreated = 0;
        ListenerHandle TransformDestroyed = 0;
        ListenerHandle RigidBodyCreated = 0;
        ListenerHandle RigidBodyDestroyed = 0;

        ListenerHandle ColliderCreated = 0;
        ListenerHandle ColliderDestroyed = 0;
    };

    void SubscribeToSceneManager();
    void UnsubscribeFromSceneManager();

    void SubscribeToWorld(World& world);
    void UnsubscribeFromWorld(World& world);

    ListenerHandle _worldCreatedListener = 0;
    ListenerHandle _worldDestroyedListener = 0;

    std::unordered_map<World*, WorldPhysicsSubscriptions> _worldSubscriptions;

    JobSystem& _jobs;
    JoltPhysicsBackend* GetPhysics(World& world) const;
};
