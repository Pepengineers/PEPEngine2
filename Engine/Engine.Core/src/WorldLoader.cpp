#include "Engine.Core/WorldLoader.h"
#include "Engine.Core/World.h"
#include "Engine.Core/Components.h"

bool WorldLoader::LoadFromFile(World& world, const std::filesystem::path& path)
{
    /* todo if file not found return 0;
    rapid yaml parsing
    create entities and components
    */
    world.SetName(path.string()); 

    
    /* example creating entities 
    
    WorldECS& ecs = world.GetECS();

    Entity e1 = ecs.CreateEntity();
    ecs.Add<NameComponent>(e1, NameComponent{ "Entity_A" });
    ecs.Add<TranslateComponent>(e1, TranslateComponent{ 0.0f, 1.0f, 2.0f });
    ecs.Add<VelocityComponent>(e1, VelocityComponent{ 1.0f, 0.0f, 0.0f });

    Entity e2 = ecs.CreateEntity();
    ecs.Add<NameComponent>(e2, NameComponent{ "Entity_B" });
    ecs.Add<TranslateComponent>(e2, TranslateComponent{ 10.0f, 20.0f, 30.0f });
    */
    return true;
}

bool WorldLoader::SaveToFile(World& world, const std::filesystem::path& path)
{
    // todo construct yaml and save
    return true;
}
