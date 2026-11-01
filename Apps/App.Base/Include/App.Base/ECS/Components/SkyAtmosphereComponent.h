#pragma once
#include "Engine.Core/ECS/Component.h"
#include "Engine.RendererDX12/GDX12Sky.h"

class SkyAtmosphereComponent : public ComponentTag
{
public:
    SkyAtmosphereComponent() = default;
    SkyAtmosphereComponent(const SkyAtmosphereComponent&) = default;
    SkyAtmosphereComponent& operator=(const SkyAtmosphereComponent& other)
    {
        if (this != &other) SetSettings(other.GetSettings());
        return *this;
    }
    const SkyAtmosphereSettings& GetSettings() const { return _settings; }
    void SetSettings(const SkyAtmosphereSettings& settings)
    {
        ValidateSkySettings(settings);
        _settings = settings;
        ++_revision;
    }
    uint64_t GetRevision() const { return _revision; }
private:
    SkyAtmosphereSettings _settings;
    uint64_t _revision = 1;
};
