#pragma once
#include "Engine.RendererDX12/GDX12GrassWind.h"
#include "Engine.Core/ECS/Component.h"
struct WindComponent : ComponentTag, GDX12WindSettings { bool Enabled = true; };
