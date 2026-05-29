#pragma once

#include "Component.h"
#include "Engine.RendererDX12/D3DHelpers.h"

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