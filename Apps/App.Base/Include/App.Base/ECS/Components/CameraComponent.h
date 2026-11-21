#pragma once

#include "Engine.Core/ECS/Component.h"
#include <cstdint>
#include "Engine.RendererDX12/D3DHelpers.h"

struct CameraComponent : ComponentTag
{
    float FOV;
    float NearPlane;
    float FarPlane;
    bool DirtyFlag;

    CameraComponent()
        : FOV(60.0f), NearPlane(0.1f), FarPlane(10000.f), DirtyFlag(true),
          ViewProj(Identity4x4()), PrevViewProjNoJitter(Identity4x4()), _CBufferIndex(0), _numFramesDirty(0)
    {
    }

private:
    friend class RenderModule;
    friend class GPUDataUpdateSystem;

    Matrix ViewProj;
    Matrix PrevViewProjNoJitter;
    std::uint32_t _CBufferIndex;
    std::uint32_t _numFramesDirty;
};
