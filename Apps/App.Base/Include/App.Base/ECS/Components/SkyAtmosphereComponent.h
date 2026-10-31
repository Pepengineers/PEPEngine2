#pragma once
#include "Engine.RendererDX12/GDX12Sky.h"
#include "Engine.Core/ECS/Component.h"
struct SkyAtmosphereComponent : ComponentTag, GDX12SkyAtmosphereSettings { bool Enabled = true; };
