#pragma once

#include "Engine.Core/ECS/Component.h"
#include <string>

struct NameComponent : ComponentTag
{
    std::string value;

    NameComponent(const std::string& inValue)
        : value(inValue)
    {
    }
};