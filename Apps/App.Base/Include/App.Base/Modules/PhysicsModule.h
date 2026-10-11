#pragma once

#include <memory>

#include "Common/GameTimer.h"
#include "Common/Module.h"
#include "Engine.Core/ECS/Entity.h"
#include "App.Base/Physics/PhysicsTypes.h"

class  World;

class JoltPhysicsBackend;

class PhysicsModule final : public Module
{
public:
    explicit PhysicsModule(GameTimer* gt);
    ~PhysicsModule() override;
    
    void Initialize() override;
    void Uninitialize() override;
    
    // create bodies
    PhysicsBodyHandle CreateBoxBody(const PhysicsBoxBodyDesc& desc);
    
    // destroy bodies
    void DestroyBody(PhysicsBodyHandle handle);
    
protected:
    void OnUpdate() override;
    void OnRender() override;
    
    bool ShouldTick() override;
    bool ShouldRender() override;

private:
    void Tick(float dt);
    
    void TryCreateBody(World& world, Entity entity);
    void TryDestroyBody(World& world, Entity entity);
    
    GameTimer* _gt;
    std::unique_ptr<JoltPhysicsBackend> _cpuPhysics;
    float _accumulatedTime = 0.0f;
    static constexpr float FixedDeltaTime = 1.0f / 60.0f;
};