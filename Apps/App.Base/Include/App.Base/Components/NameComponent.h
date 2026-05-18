#pragma once

#include "Component.h"

struct NameComponent : ComponentTag
{
    std::string value;

    NameComponent(const std::string& inValue)
        : value(inValue)
    {
    }
};