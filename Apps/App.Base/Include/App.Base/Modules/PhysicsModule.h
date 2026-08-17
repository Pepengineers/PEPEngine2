#pragma once

#include <memory>

#include "Common/GameTimer.h"
#include "Common/Module.h"

class JoltPhysicsBackend;

class PhysicsModule final : public Module
{
    explicit PhysicsModule(GameTimer* gt);
    ~PhysicsModule() override;
    
    void Initialize() override;
    void Uninitialize() override;
    
protected:
    void OnUpdate() override;
    void OnRender() override;
    
    bool ShouldTick() override;
    bool ShouldRender() override;

private:
    void Tick(float dt);
    
    GameTimer* _gt;
    std::unique_ptr<JoltPhysicsBackend> _cpuPhysics;
    float _accumulatedTime = 0.0f;
    static constexpr float FixedDeltaTime = 1.0f / 60.0f;
};