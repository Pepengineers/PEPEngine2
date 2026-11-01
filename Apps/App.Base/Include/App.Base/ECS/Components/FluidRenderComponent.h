#pragma once
#include "Engine.Core/ECS/Component.h"
#include "Engine.RendererDX12/GDX12Fluid.h"

struct FluidRenderComponent : ComponentTag, GDX12FluidComponent
{
};
