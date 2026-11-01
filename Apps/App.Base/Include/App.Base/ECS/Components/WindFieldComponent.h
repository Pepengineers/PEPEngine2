#pragma once
#include "Engine.Core/ECS/Component.h"
#include "Engine.RendererDX12/GDX12GrassWind.h"
#include <stdexcept>

struct WindFieldComponent : ComponentTag, GDX12WindSettings
{
    bool Enabled = true;
    bool Paused = false;
    uint64_t _renderOwner = 0;

    void SetSettings(const GDX12WindSettings& settings)
    {
        if (!IsValidWindSettings(settings)) throw std::invalid_argument("Invalid wind settings");
        static_cast<GDX12WindSettings&>(*this) = settings;
        Reset();
    }
    void Reset() { ++ResetRevision; }
};
