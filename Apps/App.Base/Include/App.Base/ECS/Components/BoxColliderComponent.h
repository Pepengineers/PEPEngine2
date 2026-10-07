#pragma once

#include "Engine.Core/ECS/Component.h"
#include <directxtk/SimpleMath.h>

enum class ColliderType : uint8_t
{
    Box
};

struct BoxColliderComponent : ComponentTag
{
    DirectX::SimpleMath::Vector3 HalfExtent{ 0.5f, 0.5f, 0.5f };
};