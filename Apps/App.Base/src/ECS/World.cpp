#include "App.Base/ECS/World.h"
#include "App.Base/ECS/WorldLoader.h"

bool World::Load()
{
    return WorldLoader::LoadFromFile(*this, _desc.WorldFilePath);
}

bool World::Save(std::filesystem::path path)
{
    return WorldLoader::SaveToFile(*this, path);
}

void World::Tick(float dt) // not implemented
{
    if (_bPaused) { return; }

    float scaledDt = dt * _timeScale;

    // scripting here
}

void World::SetTimeScale(float scale) { _timeScale = scale; }

float World::GetTimeScale() const { return _timeScale; }

void World::SetPaused(bool paused) { _bPaused = paused; }

bool World::IsPaused() const { return _bPaused; }

void World::SetName(std::string name) {
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