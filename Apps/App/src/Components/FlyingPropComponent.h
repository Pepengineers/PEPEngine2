#pragma once

#include "Engine.Core/ECS/Component.h"
#include <directxtk/SimpleMath.h>
#include <cmath>

struct FlyingPropComponent : ComponentTag
{
    DirectX::SimpleMath::Vector3 Origin = DirectX::SimpleMath::Vector3::Zero;
    float Time = 0;
    float Phase = 0;
    float Speed = 0.5f;
    float HorizontalRadius = 15;
    float VerticalLift = 20;

    DirectX::SimpleMath::Vector3 OffsetAt(float seconds) const
    {
        const float angle = seconds * Speed;
        return {
            HorizontalRadius * (std::sin(angle + Phase) - std::sin(Phase)),
            VerticalLift * (1 - std::cos(angle)),
            HorizontalRadius * (std::cos(angle + Phase) - std::cos(Phase))
        };
    }
};
