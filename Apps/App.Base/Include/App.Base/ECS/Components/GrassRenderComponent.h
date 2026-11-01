#pragma once
#include "Engine.RendererDX12/GDX12GrassWind.h"
#include "Engine.RendererDX12/GDX12Material.h"
#include "Engine.Core/ECS/Component.h"
struct GrassRenderComponent : ComponentTag, GDX12GrassSettings
{
    bool Enabled = true;
    GDX12Material* Material = nullptr;
};
