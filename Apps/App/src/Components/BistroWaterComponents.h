#pragma once
#include "App.Base/ECS/World.h"
#include "Components/FluidRenderComponent.h"

inline void RegisterBistroWaterComponents(World& world)
{
    world.GetECS().RegisterComponent<FluidRenderComponent>();
}
