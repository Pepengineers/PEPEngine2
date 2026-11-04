#pragma once
#include "Engine.Core/ECS/Component.h"
#include "Engine.Core/ECS/Entity.h"
#include "Rendering/Grass/GDX12GrassWind.h"
#include <stdexcept>

class GDX12Material;
struct GrassRenderComponent : ComponentTag, GDX12GrassSettings
{
    bool Enabled = true;
    GDX12Material* Material = nullptr;
    Entity WindEntity = InvalidEntity;
    UINT ResetRevision = 0;
    uint64_t _renderOwner = 0;

    void SetSettings(const GDX12GrassSettings& settings)
    {
        if (!IsValidGrassSettings(settings)) throw std::invalid_argument("Invalid grass settings");
        static_cast<GDX12GrassSettings&>(*this) = settings;
        ResetHistory();
    }
    void ResetHistory() { ++ResetRevision; }
};
