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

    //
    //Loading Neccessary Assets
    //
    auto& assetManager = Engine::Core::AssetManager::GetInstance();
    auto renderModule = BenchmarkEngine::GetLocator().GetModule<RenderModule>();

    //Textures: 
    //this should probably be done via TextureHandle
    auto HeadTexture = assetManager.LoadTexture("african_head_diffuse.dds");

    //this should be automated via events
    renderModule->CreateTexture("HeadTexture", HeadTexture);

    //Materials: 
    auto HeadMaterial = renderModule->CreateMaterial("HeadMaterial");
    HeadMaterial->Metallic = 0.f;
    HeadMaterial->Roughness = 0.8f;
    HeadMaterial->Diffuse = renderModule->GetTextureByName("HeadTexture");

    auto material2 = renderModule->CreateMaterial("test1");
    material2->Metallic = 0.f;
    material2->Roughness = 1.f;

    //Meshes: 
    Engine::Core::MeshAssetLocator locator = {};
    locator.SourcePath = "african_head.obj";

    Engine::Core::MeshHandle HeadMeshHandle;
    const Engine::Core::Mesh* HeadMesh = assetManager.Meshes().Load(locator, HeadMeshHandle);

    //this should be automated via events
    renderModule->SubmitMesh(HeadMesh, HeadMeshHandle);

    locator.SourcePath = "Svidetel.fbx";

    Engine::Core::MeshHandle SvMeshHandle;
    const Engine::Core::Mesh* SvMesh = assetManager.Meshes().Load(locator, SvMeshHandle);

    //this should be automated via events
    renderModule->SubmitMesh(SvMesh, SvMeshHandle);

    //
    // Creating Entities and components
    //
    WorldECS& ecs = world.GetECS();
    
    auto en1 = ecs.CreateEntity();
    en1.AddComponent<NameComponent>("en1");
    en1.AddComponent<TransformComponent>(Vector3(0.f, 0.f, 0.f));

    //this should be automated via events
    renderModule->OnTransformComponentCreated(en1.GetComponent<TransformComponent>());

    //en1.AddComponent<VelocityComponent>(1.0f, 0.0f, 0.0f);

    std::vector<GDX12Material*> HeadMaterials = { HeadMaterial };
    en1.AddComponent<StaticMeshRenderComponent>(HeadMeshHandle, HeadMaterials);

    auto en2 = ecs.CreateEntity();
    en2.AddComponent<NameComponent>("en2");
    en2.AddComponent<TransformComponent>(Vector3(10.f, 0.f, 5.f));

    //this should be automated via events
    renderModule->OnTransformComponentCreated(en2.GetComponent<TransformComponent>());

    en2.AddComponent<VelocityComponent>(-0.5f, 0.0f, 0.25f);

    auto marker = ecs.CreateEntity();
    marker.AddComponent<NameComponent>("marker");
    marker.AddComponent<TransformComponent>(Vector3(3.f, 7.f, -1.f));

    //this should be automated via events
    renderModule->OnTransformComponentCreated(marker.GetComponent<TransformComponent>());

    auto camera = ecs.CreateEntity();
    camera.AddComponent<NameComponent>("MainCamera");
    camera.AddComponent<TransformComponent>(Vector3(0.f, 0.f, 3.f), Vector3(0.f, 180.f, 0.f));

    //this should be automated via events
    renderModule->OnTransformComponentCreated(camera.GetComponent<TransformComponent>());

    camera.AddComponent<CameraComponent>();

    //this should be automated via events
    renderModule->OnCameraComponentCreated(camera.GetComponent<CameraComponent>());

    world.ActiveCamera = camera;
    
    return true;
}

bool WorldLoader::SaveToFile(World& world, const std::filesystem::path& path)
{
    // todo construct yaml and save
    return true;
}
