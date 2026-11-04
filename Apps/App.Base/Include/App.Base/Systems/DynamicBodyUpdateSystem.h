#pragma once

#include "Engine.Core/System.h"
#include "App.Base/ECS/World.h"
#include "App.Base/Modules/PhysicsModule.h"

class DynamicBodyUpdateSystem final : public System
{
public:
    explicit DynamicBodyUpdateSystem(PhysicsModule& physics)
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
            [this, &world](Entity entity, RigidBodyComponent& rigidBody, TransformComponent& transform)
            {
                if (rigidBody.MotionType != PhysicsMotionType::Dynamic || !rigidBody.Body.IsValid()) { return; }

                DirectX::SimpleMath::Vector3 position;
                DirectX::SimpleMath::Quaternion rotation;

                if (!_physics.TryGetBodyTransform(world, rigidBody.Body, position, rotation)) { return; }

                const auto radians = rotation.ToEuler();

                const DirectX::SimpleMath::Vector3 degrees(
                    DirectX::XMConvertToDegrees(radians.x),
                    DirectX::XMConvertToDegrees(radians.y),
                    DirectX::XMConvertToDegrees(radians.z));

                if (transform.Location == position && transform.Rotation == degrees) { return; }

                transform.Location = position;
                transform.Rotation = degrees;
                transform.DirtyFlag = true;
            });
    }

    SystemUpdatePhase GetUpdatePhase() const override
    {
        return SystemUpdatePhase::FixedPostPhysics;
    }

private:
    PhysicsModule& _physics;
};
