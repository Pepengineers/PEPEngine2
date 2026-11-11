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

Entity World::GetParent(Entity child) const
{
    if (!_ecs.Has<ParentComponent>(child))
    {
        return InvalidEntity;
    }
    
    const Entity parent = _ecs.Get<ParentComponent>(child).Parent;
    return _ecs.IsAlive(parent) ? parent : InvalidEntity;
}

bool World::CanSetParent(Entity child, Entity parent) const
{
    if (!_ecs.IsAlive(child) || (parent != InvalidEntity && !_ecs.IsAlive(parent)) || GetParent(child) == parent)
    {
        return false;
    }
    
    //to remove cycling
    for (Entity ancestor = parent; ancestor != InvalidEntity; ancestor = GetParent(ancestor))
    {
        if (ancestor == child)
        {
            return false;
        }
    }
    
    return true;
}

bool World::SetParent(Entity child, Entity parent)
{
    if (!CanSetParent(child, parent))
    {
        return false;
    }
    
    if (parent == InvalidEntity)
    {
        _ecs.Remove<ParentComponent>(child);
    }
    else
    {
        _ecs.Add<ParentComponent>(child, parent);
    }
    
    return true;
}

WorldECS& World::GetECS()
{
    return _ecs;
}

const WorldECS& World::GetECS() const
{
    return _ecs;
}