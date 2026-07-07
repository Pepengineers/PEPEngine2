#pragma once

#include "Engine.Core/ECS/Component.h"
#include <cstdint>
#include "Engine.RendererDX12/D3DHelpers.h"

struct CameraComponent : ComponentTag
{
    float FOV;
    float NearPlane;
    float FarPlane;
    Matrix ViewProj;
    Matrix PrevViewProjNoJitter;

    bool DirtyFlag;

    CameraComponent()
        : DirtyFlag(true), _numFramesDirty(0), _CBufferIndex(0), 
        FOV(60.0f), NearPlane(0.1f), FarPlane(10000.f), ViewProj(Identity4x4()), PrevViewProjNoJitter(Identity4x4())
    {
    }

    std::uint32_t _CBufferIndex;
    std::uint32_t _numFramesDirty;
};