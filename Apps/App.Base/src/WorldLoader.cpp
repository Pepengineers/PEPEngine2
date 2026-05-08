#include "App.Base/WorldLoader.h"
#include "App.Base/World.h"
#include "App.Base/Components.h"

bool WorldLoader::LoadFromFile(World& world, const std::filesystem::path& path)
{
    /* todo if file not found return 0;
    rapid yaml parsing
    create entities and components
    */
    world.SetName(path.string()); 

    
    /* example creating entities 
    
    WorldECS& ecs = world.GetECS();
    const std::string worldName = path.stem().string();

    auto player = ecs.CreateEntity();
    player.AddComponent<NameComponent>(worldName + "_Player");
    player.AddComponent<TranslateComponent>(0.0f, 1.0f, 2.0f);
    player.AddComponent<VelocityComponent>(1.0f, 0.0f, 0.0f);

    auto enemy = ecs.CreateEntity();
    enemy.AddComponent<NameComponent>(worldName + "_Enemy");
    enemy.AddComponent<TranslateComponent>(10.0f, 0.0f, 5.0f);
    enemy.AddComponent<VelocityComponent>(-0.5f, 0.0f, 0.25f);

    auto marker = ecs.CreateEntity();
    marker.AddComponent<NameComponent>(worldName + "_Marker");
    marker.AddComponent<TranslateComponent>(3.0f, 7.0f, -1.0f);
    */
    WorldECS& ecs = world.GetECS();
    
    auto en1 = ecs.CreateEntity();
    en1.AddComponent<NameComponent>("en1");
    en1.AddComponent<TranslateComponent>(0.0f, 1.0f, 2.0f);
    en1.AddComponent<VelocityComponent>(1.0f, 0.0f, 0.0f);

    auto en2 = ecs.CreateEntity();
    en2.AddComponent<NameComponent>("en2");
    en2.AddComponent<TranslateComponent>(10.0f, 0.0f, 5.0f);
    en2.AddComponent<VelocityComponent>(-0.5f, 0.0f, 0.25f);

    auto marker = ecs.CreateEntity();
    marker.AddComponent<NameComponent>("marker");
    marker.AddComponent<TranslateComponent>(3.0f, 7.0f, -1.0f);
    
    return true;
}

bool WorldLoader::SaveToFile(World& world, const std::filesystem::path& path)
{
    // todo construct yaml and save
    return true;
}
