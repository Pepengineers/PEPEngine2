#pragma once
#include "Engine.Core/ECS/Component.h"
#include "Engine.RendererDX12/GDX12Sky.h"

class SkyBoxComponent : public ComponentTag
{
public:
    SkyBoxComponent() = default;
    SkyBoxComponent(const SkyBoxComponent&) = default;
    SkyBoxComponent& operator=(const SkyBoxComponent& other)
    {
        if (this != &other) SetSettings(other.GetSettings());
        return *this;
    }
    const SkyBoxSettings& GetSettings() const { return _settings; }
    void SetSettings(const SkyBoxSettings& settings)
    {
        ValidateSkySettings(settings);
        _settings = settings;
        ++_revision;
    }
    uint64_t GetRevision() const { return _revision; }
private:
    SkyBoxSettings _settings;
    uint64_t _revision = 1;
};
