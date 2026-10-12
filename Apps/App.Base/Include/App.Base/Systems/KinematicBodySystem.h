#pragma once

#include "Engine.Core/System.h"
#include "App.Base/ECS/World.h"
#include "App.Base/Modules/PhysicsModule.h"

class KinematicBodySystem final : public System
{
public:
    explicit KinematicBodySystem(PhysicsModule& physics)
        : _physics(physics)
    {
    }

    void Tick(World& world, float dt) override
    {
        auto& ecs = world.GetECS();
        ecs.ForEach<RigidBodyComponent, TransformComponent>(
            [this, dt](Entity, RigidBodyComponent& rigidBody, TransformComponent& transform)
            {
                if (rigidBody.MotionType != PhysicsMotionType::Kinematic || !rigidBody.Body.IsValid()) { return; }
                // TODO make quats everywhere
                const auto rotation = DirectX::SimpleMath::Quaternion::CreateFromYawPitchRoll(
                    DirectX::XMConvertToRadians(transform.Rotation.y),
                    DirectX::XMConvertToRadians(transform.Rotation.x),
                    DirectX::XMConvertToRadians(transform.Rotation.z));

                _physics.MoveKinematicBody(rigidBody.Body, transform.Location, rotation, dt);
            });
    }

    SystemUpdatePhase GetUpdatePhase() const override
    {
        return SystemUpdatePhase::FixedPrePhysics;
    }
    
private:
    PhysicsModule& _physics;
};