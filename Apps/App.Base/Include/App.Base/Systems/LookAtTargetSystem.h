#pragma once

#include "Engine.Core/System.h"
#include "App.Base/World.h"

inline Vector3 MakeEulerFromForwardLH(Vector3 forward)
{
    if (forward.LengthSquared() < 0.0001f) return Vector3(0.0f, 0.0f, 0.0f);

    forward.Normalize();

    const float yaw = std::atan2(forward.x, forward.z);
    const float horizontalLength = std::sqrt(forward.x * forward.x + forward.z * forward.z);
    const float pitch = std::atan2(-forward.y, horizontalLength);

    constexpr float RadToDeg = 57.295779513f;

    return Vector3(
        pitch * RadToDeg,
        yaw * RadToDeg,
        0.0f
    );
}

class LookAtTargetSystem final : public System
{
public:
    void Tick(World& world, float dt) override
    {
        WorldECS& ecs = world.GetECS();

        ecs.ForEach<TransformComponent, LookAtTargetComponent>(
            [&](Entity entity, TransformComponent& transform, LookAtTargetComponent& lookAt)
            {
                if (!lookAt.bEnabled || lookAt.TargetEntity == InvalidEntity) { return; }

                auto targetEntity = ecs.GetEntityHandle(lookAt.TargetEntity);

                if (!targetEntity.HasComponent<TransformComponent>()) { return; }

                TransformComponent& targetTransform = targetEntity.GetComponent<TransformComponent>();

                Vector3 direction = targetTransform.Location - transform.Location;

                if (direction.LengthSquared() < 0.0001f) { return; }

                transform.Rotation = MakeEulerFromForwardLH(direction);
                transform.DirtyFlag = true;
            });
    }
};