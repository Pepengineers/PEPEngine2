#pragma once

#include "Engine.Core/ECS/Component.h"
#include <directxtk/SimpleMath.h>
#include <vector>

using DirectX::SimpleMath::Vector3;

struct SplinePoint
{
    Vector3 Position;
    Vector3 ArriveTangent;
    Vector3 LeaveTangent;
};

struct SplineCurveComponent : ComponentTag
{
    std::vector<SplinePoint> Points;
    bool bLoop = false;
    
    SplineCurveComponent() = default;
    
    SplineCurveComponent(bool bLoop, std::vector<SplinePoint> Points)
        : bLoop(bLoop), Points(Points)
    {
    }
};