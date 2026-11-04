#pragma once

#include "Engine.Core/System.h"
#include "App.Base/ECS/World.h"
#include "App.Base/Modules/PhysicsModule.h"

class KinematicBodyUpdateSystem final : public System
{
public:
    explicit KinematicBodyUpdateSystem(PhysicsModule& physics)
        : _physics(physics)
    {
    }

    void Tick(World& world, float dt) override
    { TickRange(world, dt, 0, GetWorkCount(world)); }

protected:
    size_t GetWorkCount(World& world) const override { return world.GetECS().GetPool<RigidBodyComponent>().Size(); }
    void TickRange(World& world, float dt, size_t begin, size_t end) override
    {
        auto& ecs = world.GetECS();
        ecs.ForEachRange<RigidBodyComponent, TransformComponent>(begin, end,
            [this, &world, dt](Entity, RigidBodyComponent& rigidBody, TransformComponent& transform)
            {
                if (rigidBody.MotionType != PhysicsMotionType::Kinematic || !rigidBody.Body.IsValid()) { return; }

                const auto rotation = DirectX::SimpleMath::Quaternion::CreateFromYawPitchRoll(
                    DirectX::XMConvertToRadians(transform.Rotation.y),
                    DirectX::XMConvertToRadians(transform.Rotation.x),
                    DirectX::XMConvertToRadians(transform.Rotation.z));

                _physics.MoveKinematicBody(world, rigidBody.Body, transform.Location, rotation, dt);
            });
    }

    SystemUpdatePhase GetUpdatePhase() const override
    {
        return SystemUpdatePhase::FixedPrePhysics;
    }

private:
    PhysicsModule& _physics;
};
