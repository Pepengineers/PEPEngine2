#pragma once

#include "Component.h"
#include "Engine.RendererDX12/D3DHelpers.h"

struct TransformComponent : ComponentTag
{
    Vector3 Location;
    Vector3 Rotation;
    Vector3 Scale;

    bool DirtyFlag;

    TransformComponent(Vector3 location = Vector3(0.f, 0.f, 0.f), 
        Vector3 rotation = Vector3(0.f, 0.f, 0.f), Vector3 scale = Vector3(1.f, 1.f, 1.f)) 
        : Location(location), DirtyFlag(true), _numFramesDirty(0),
        Rotation(rotation), Scale(scale), _CBufferIndex(0)
    {
    }

    UINT _CBufferIndex;
    UINT _numFramesDirty;
};