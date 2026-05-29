#include "App.Base/WorldLoader.h"
#include "App.Base/World.h"

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

    auto SvTexture = assetManager.LoadTexture("friazino_diff.png");

    //this should be automated via events
    renderModule->CreateTexture("SvTexture", SvTexture);

    //Materials: 
    auto HeadMaterial = renderModule->CreateMaterial("HeadMaterial");
    HeadMaterial->Metallic = 0.f;
    HeadMaterial->Roughness = 0.8f;
    HeadMaterial->Diffuse = renderModule->GetTextureByName("HeadTexture");

    auto SvMaterial = renderModule->CreateMaterial("SvMaterial");
    SvMaterial->Metallic = 0.1f;
    SvMaterial->Roughness = 1.f;
    SvMaterial->Diffuse = renderModule->GetTextureByName("SvTexture");

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
    en1.AddComponent<TransformComponent>(Vector3(0.0f, 0.f, 0.f));
    en1.AddComponent<CircleMovementComponent>(1,1);

    std::vector<GDX12Material*> HeadMaterials = { HeadMaterial };
    en1.AddComponent<StaticMeshRenderComponent>(HeadMeshHandle, HeadMaterials);
    
    auto splineEntity = ecs.CreateEntity();
    splineEntity.AddComponent<NameComponent>("en2_spline");

    std::vector<SplinePoint> splinePoints =
    {
        {
            Vector3(10.f, -5.f, -20.f),
            Vector3(0.f, 0.f, 0.f),
            Vector3(-5.f, 0.f, 8.f)
        },
        {
            Vector3(0.f, -2.f, -12.f),
            Vector3(5.f, 0.f, -8.f),
            Vector3(-5.f, 6.f, 8.f)
        },
        {
            Vector3(-10.f, 2.f, -22.f),
            Vector3(5.f, -6.f, -8.f),
            Vector3(-5.f, 4.f, -8.f)
        },
        {
            Vector3(-20.f, -3.f, -16.f),
            Vector3(5.f, -4.f, 8.f),
            Vector3(-5.f, 0.f, 6.f)
        },
        {
            Vector3(-30.f, -5.f, -25.f),
            Vector3(5.f, 0.f, -6.f),
            Vector3(0.f, 0.f, 0.f)
        }
    };

    splineEntity.AddComponent<SplineCurveComponent>(
        false,
        splinePoints
    );

    auto en2 = ecs.CreateEntity();
    en2.AddComponent<NameComponent>("en2");
    en2.AddComponent<TransformComponent>(Vector3(10.f, -5.f, -20.f), Vector3(0.f, 0.f, 0.f),
    Vector3(0.1f, 0.1f, 0.1f));

    std::vector<GDX12Material*> SvMaterials = { SvMaterial };
    en2.AddComponent<StaticMeshRenderComponent>(SvMeshHandle, SvMaterials);

    en2.AddComponent<SplineFollowComponent>(
        splineEntity.GetId(),
        8.0f,   // duration
        false,  // bLoop
        true    // bPlaying
    );

    auto marker = ecs.CreateEntity();
    marker.AddComponent<NameComponent>("marker");
    marker.AddComponent<TransformComponent>(Vector3(3.f, 7.f, -1.f));

    auto camera = ecs.CreateEntity();
    camera.AddComponent<NameComponent>("MainCamera");
    camera.AddComponent<TransformComponent>(Vector3(0.f, 0.f, 3.f), Vector3(0.f, 180.f, 0.f));

    camera.AddComponent<CameraComponent>();
    camera.AddComponent<LookAtTargetComponent>(en1.GetId());

    world.ActiveCamera = camera;
    
    return true;
}

bool WorldLoader::SaveToFile(World& world, const std::filesystem::path& path)
{
    // todo construct yaml and save
    return true;
}
