#pragma once
#include "Engine.Core/ECS/Component.h"
#include "Rendering/Water/GDX12Fluid.h"

struct FluidRenderComponent : ComponentTag, GDX12FluidComponent
{
    uint64_t _renderOwner = 0;
};
