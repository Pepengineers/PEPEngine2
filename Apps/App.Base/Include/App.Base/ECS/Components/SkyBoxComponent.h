#pragma once
#include "Engine.RendererDX12/GDX12Sky.h"
#include "Engine.Core/ECS/Component.h"
struct SkyBoxComponent : ComponentTag, GDX12SkyBoxSettings { bool Enabled = true; };
