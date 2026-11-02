#include "App.Base/ECS/World.h"
#include "App.Base/ECS/WorldLoader.h"

bool World::Load()
{
    return WorldLoader::LoadFromFile(*this, _desc.WorldFilePath);
}

bool World::Save(const std::filesystem::path& path)
{
    return WorldLoader::SaveToFile(*this, path);
}

void World::Tick(const float dt) // not implemented
{
    if (_bPaused) { return; }

    float scaledDt = dt * _timeScale;

    // scripting here
}

void World::SetTimeScale(const float scale) { _timeScale = scale; }

float World::GetTimeScale() const { return _timeScale; }

void World::SetPaused(const bool paused) { _bPaused = paused; }

bool World::IsPaused() const { return _bPaused; }

void World::SetName(const std::string& name) {
    _desc.Name = name;
}

const std::filesystem::path& World::GetPath() const { return _desc.WorldFilePath; }

WorldECS& World::GetECS()
{
    return _ecs;
}

const WorldECS& World::GetECS() const
{
    return _ecs;
}
void World::SetSkySource(const SkySource source, const Entity entity)
{
    if (source != SkySource::Off && source != SkySource::SkyBox && source != SkySource::SkyAtmosphere)
        throw std::invalid_argument("Unknown sky source");
    if ((source == SkySource::SkyBox && !_ecs.Has<SkyBoxComponent>(entity)) ||
        (source == SkySource::SkyAtmosphere && !_ecs.Has<SkyAtmosphereComponent>(entity)))
        throw std::invalid_argument("Selected sky entity has no matching component");
    _skySource = source;
    _skyEntity = source == SkySource::Off ? InvalidEntity : entity;
    ++_skyRevision;
}
