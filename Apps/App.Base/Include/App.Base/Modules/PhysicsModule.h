#pragma once

#define NOMINMAX

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

class PhysicsModule final : public Module
{
public:
    explicit PhysicsModule(GameTimer* gt);
    ~PhysicsModule() override;
    
    void Initialize() override;
    void Uninitialize() override;
    
    void Step(float dt);
    
    // create bodies
    PhysicsBodyHandle CreateBoxBody(const PhysicsBoxBodyDesc& desc);
    
    // destroy bodies
    void DestroyBody(PhysicsBodyHandle handle);
    
    bool TryGetBodyTransform(PhysicsBodyHandle handle, DirectX::SimpleMath::Vector3& outPosition, DirectX::SimpleMath::Quaternion& outRotation) const;
    
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
    
    GameTimer* _gt;
    std::unique_ptr<JoltPhysicsBackend> _cpuPhysics;
};