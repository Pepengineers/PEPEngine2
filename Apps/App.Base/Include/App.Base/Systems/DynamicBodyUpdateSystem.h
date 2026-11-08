#pragma once

#include "Engine.Core/System.h"
#include "App.Base/ECS/World.h"
#include "App.Base/Modules/PhysicsModule.h"
#include <cmath>

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
    // Small pose-read tasks cost more to schedule than they save in parallel work.
    size_t GetBatchSize(const JobSystem&) const override { return 256; }
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

                if (!rigidBody._hasPhysicsRotation || rigidBody._lastPhysicsRotation != rotation)
                {
                    rigidBody._lastPhysicsEuler = RotationDegrees(rotation);
                    rigidBody._lastPhysicsRotation = rotation;
                    rigidBody._hasPhysicsRotation = true;
                }
                const auto& degrees = rigidBody._lastPhysicsEuler;

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
    static DirectX::SimpleMath::Vector3 RotationDegrees(const DirectX::SimpleMath::Quaternion& q)
    {
        // Float Euler decomposition near vertical pitch magnifies tiny quaternion errors at baked mesh pivots.
        const double x=q.x, y=q.y, z=q.z, w=q.w;
        const double scale=2/(x*x+y*y+z*z+w*w);
        const double m31=scale*(x*z+y*w), m32=scale*(y*z-x*w), m33=1-scale*(x*x+y*y);
        const double cy=std::hypot(m31,m33);
        const double pitch=std::atan2(-m32,cy);
        double yaw=0, roll;
        if(cy>1e-12)
        {
            yaw=std::atan2(m31,m33);
            roll=std::atan2(scale*(x*y+z*w),1-scale*(x*x+z*z));
        }
        else roll=std::atan2(-scale*(x*y-z*w),1-scale*(y*y+z*z));
        constexpr double degrees=180/3.14159265358979323846;
        return {static_cast<float>(pitch*degrees),static_cast<float>(yaw*degrees),static_cast<float>(roll*degrees)};
    }

    PhysicsModule& _physics;
};
