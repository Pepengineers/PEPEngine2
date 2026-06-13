#pragma once

#include "Engine.Core/System.h"
#include "App.Base/ECS/World.h"

#include <algorithm>
#include <cmath>

inline Vector3 EvaluateBezier(
    const Vector3& p0,
    const Vector3& p1,
    const Vector3& p2,
    const Vector3& p3,
    float t)
{
    t = std::clamp(t, 0.0f, 1.0f);

    const float u = 1.0f - t;

    return
        p0 * (u * u * u) +
        p1 * (3.0f * u * u * t) +
        p2 * (3.0f * u * t * t) +
        p3 * (t * t * t);
}

inline Vector3 EvaluateSplineSegment(
    const SplinePoint& a,
    const SplinePoint& b,
    float localT)
{
    const Vector3 p0 = a.Position;
    const Vector3 p1 = a.Position + a.LeaveTangent;
    const Vector3 p2 = b.Position + b.ArriveTangent;
    const Vector3 p3 = b.Position;

    return EvaluateBezier(p0, p1, p2, p3, localT);
}

inline Vector3 EvaluateSpline(const SplineCurveComponent& spline, float t)
{
    const int pointCount = static_cast<int>(spline.Points.size());

    if (pointCount == 0) return Vector3(0.0f, 0.0f, 0.0f);

    if (pointCount == 1) return spline.Points[0].Position;

    t = std::clamp(t, 0.0f, 1.0f);

    const int segmentCount = spline.bLoop ? pointCount : pointCount - 1;

    float scaledT = t * static_cast<float>(segmentCount);

    int segmentIndex = static_cast<int>(std::floor(scaledT));

    if (segmentIndex >= segmentCount) segmentIndex = segmentCount - 1;

    const float localT = scaledT - static_cast<float>(segmentIndex);

    const int currentPointIndex = segmentIndex;

    int nextPointIndex = segmentIndex + 1;

    if (spline.bLoop) nextPointIndex %= pointCount;

    return EvaluateSplineSegment(
        spline.Points[currentPointIndex],
        spline.Points[nextPointIndex],
        localT
    );
}

class SplineFollowSystem final : public System
{
public:
    void Tick(World& world, float dt) override
    {
        WorldECS& ecs = world.GetECS();
        
        ecs.ForEach<TransformComponent, SplineFollowComponent>(
            [&](Entity entity, TransformComponent& transform, SplineFollowComponent& follow)
            {
                if (!follow.bPlaying) return;
                
                auto& curve = ecs.GetEntityHandle(follow.CurveEntity).GetComponent<SplineCurveComponent>();

                follow.Time += dt;

                float t = follow.Time / follow.Duration;

                if (follow.bLoop)
                {
                    t = std::fmod(t, 1.0f);
                }
                else
                {
                    t = std::clamp(t, 0.0f, 1.0f);

                    if (t >= 1.0f)
                        follow.bPlaying = false;
                }

                transform.Location = EvaluateSpline(curve, t);
                transform.DirtyFlag = true;
            });
    }
};