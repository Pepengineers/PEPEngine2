#include "App.Base/WorldLoader.h"
#include "App.Base/World.h"
#include "App.Base/Components.h"

#include "Engine.Core/AssetManager.h"
#include "Engine.Core/BenchmarkEngine.h"
#include "App.Base/Modules/RenderModule.h"

bool WorldLoader::LoadFromFile(World& world, const std::filesystem::path& path)
{
    /* todo if file not found return 0;
    rapid yaml parsing
    create entities and components
    */
    world.SetName(path.string()); 

    //Loading Neccessary Assets
    auto& assetManager = Engine::Core::AssetManager::GetInstance();
    auto renderModule = BenchmarkEngine::GetLocator().GetModule<RenderModule>();

    auto HeadTexture = assetManager.LoadTexture("african_head_diffuse.dds");
    
    //this should be automated via events
    renderModule->CreateTexture("HeadTexture", HeadTexture);

    auto HeadMaterial = renderModule->CreateMaterial("HeadMaterial");
    HeadMaterial->Metallic = 0.f;
    HeadMaterial->Roughness = 0.8f;
    HeadMaterial->Diffuse = renderModule->GetTextureByName("HeadTexture");

    auto material2 = renderModule->CreateMaterial("test1");
    material2->Metallic = 0.f;
    material2->Roughness = 1.f;

    //These will be automatically uploaded onto GPU
    auto HeadMesh = assetManager.LoadMesh("african_head.obj");

    // Creating Entities and components
    WorldECS& ecs = world.GetECS();
    
    auto en1 = ecs.CreateEntity();
    en1.AddComponent<NameComponent>("en1");
    en1.AddComponent<TransformComponent>(Vector3(0.f, 1.f, 2.f));
    en1.AddComponent<VelocityComponent>(1.0f, 0.0f, 0.0f);

    auto en2 = ecs.CreateEntity();
    en2.AddComponent<NameComponent>("en2");
    en2.AddComponent<TransformComponent>(Vector3(10.f, 0.f, 5.f));
    en2.AddComponent<VelocityComponent>(-0.5f, 0.0f, 0.25f);

    auto marker = ecs.CreateEntity();
    marker.AddComponent<NameComponent>("marker");
    marker.AddComponent<TransformComponent>(Vector3(3.f, 7.f, -1.f));
    
    return true;
}

bool WorldLoader::SaveToFile(World& world, const std::filesystem::path& path)
{
    // todo construct yaml and save
    return true;
}
