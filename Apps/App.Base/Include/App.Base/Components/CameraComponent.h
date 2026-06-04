#pragma once

#include "Component.h"
#include "Engine.RendererDX12/D3DHelpers.h"

struct CameraComponent : ComponentTag
{
    float FOV;
    float NearPlane;
    float FarPlane;

    bool DirtyFlag;

    CameraComponent()
        : DirtyFlag(true), _numFramesDirty(0), _CBufferIndex(0), 
        FOV(45), NearPlane(0.1f), FarPlane(10000.f)
    {
    }

    UINT _CBufferIndex;
    UINT _numFramesDirty;
};