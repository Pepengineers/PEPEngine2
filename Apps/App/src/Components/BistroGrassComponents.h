#pragma once
#include "App.Base/ECS/World.h"
#include "Components/GrassRenderComponent.h"
#include "Components/WindFieldComponent.h"

inline void RegisterBistroGrassComponents(World& world)
{
    world.GetECS().RegisterComponent<GrassRenderComponent>();
    world.GetECS().RegisterComponent<WindFieldComponent>();
}
