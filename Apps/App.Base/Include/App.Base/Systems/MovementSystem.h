#pragma once

#include "Engine.Core/System.h"
#include "App.Base/ECS/World.h"

#include <cmath>

class MovementSystem : public System
{
public:
    explicit MovementSystem(SystemUpdatePhase phase = SystemUpdatePhase::FrameUpdate)
        : _phase(phase)
    {
    }

    SystemUpdatePhase GetUpdatePhase() const override
    {
        return _phase;
    }

    void Tick(World& world, float dt) override
    {
        WorldECS& ecs = world.GetECS();
        const bool fixedStep = _phase == SystemUpdatePhase::FixedPrePhysics;
        ecs.ForEach<VelocityComponent, TransformComponent>(
            [&](Entity entity, const VelocityComponent& velocity, TransformComponent& transform)
            {
                const bool hasBody = ecs.Has<RigidBodyComponent>(entity);
                if (hasBody != fixedStep) { return; }
                if (hasBody)
                {
                    const auto& body = ecs.Get<RigidBodyComponent>(entity);
                    if (body.MotionType != PhysicsMotionType::Kinematic || !body.Body.IsValid()) { return; }
                }

                const Vector3& angular = velocity.AngularVelocity;
                const float speed = angular.Length();
                const bool rotating = speed > 0.0f && std::isfinite(speed);
                if (!rotating && velocity.Velocity == Vector3::Zero) { return; }

                transform.Location += velocity.Velocity * dt;
                if (rotating)
                {
                    using DirectX::SimpleMath::Quaternion;
                    const auto delta = Quaternion::CreateFromAxisAngle(angular / speed, DirectX::XMConvertToRadians(speed) * dt); // TODO make quats everywhere
                    const auto orientation = Quaternion::CreateFromYawPitchRoll(transform.Rotation * DirectX::XMConvertToRadians(1.0f));
                    transform.Rotation = (orientation * delta).ToEuler() * DirectX::XMConvertToDegrees(1.0f);
                }
                transform.DirtyFlag = true;
            });
    }

private:
    SystemUpdatePhase _phase;
};
