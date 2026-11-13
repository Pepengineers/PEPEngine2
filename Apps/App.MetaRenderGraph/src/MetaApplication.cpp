#include "MetaApplication.h"
#include "MetaRenderPipeline.h"
#include "App.Base/App.h"
#include "Common/RuntimePaths.h"
#include "Engine.Core/ConsoleModule.h"
#include "Engine.Core/Logging/Logger.h"
#include "Engine.Core/AssetManager.h"
#include <d3d12sdklayers.h>
#include <fstream>
#include <stdexcept>

void RunDirectExecutionGpuTests(GDX12Device&);
void CheckGeometryGpuEvidence(const std::filesystem::path&);

namespace
{
    void Require(bool value, const char* message)
    {
        if (!value) throw std::runtime_error(message);
    }

    // Runs before BenchmarkEngine's constructor reads and locks startup CVars.
    // Both files belong exclusively to this executable's output directory.
    void PrepareStartup()
    {
        const auto root = Engine::Core::GetExecutableDirectory();
        std::filesystem::create_directories(root / "Config");
        std::filesystem::copy_file(root / "Configs/Startup.ini", root / "Config/Engine.ini",
            std::filesystem::copy_options::overwrite_existing);
        // Initialize the registry now so the invariant is checked before any device is opened.
        ConsoleModule console;
        console.Initialize();
        Require(console.FindConsoleVariable(L"Render.PrimaryAdapter").GetInt() == 0, "Unexpected primary adapter");
        Require(console.FindConsoleVariable(L"Render.Particles.SecondaryAdapter").GetInt() == -1, "Particle offload must be disabled");
        Require(!console.FindConsoleVariable(L"Render.Particles.AsyncCompute").GetBool(), "Particle async must be disabled");
        Require(!console.FindConsoleVariable(L"Render.AO.AsyncCompute").GetBool(), "AO async must be disabled");
        IConsoleVariable* water = nullptr;
        Require(!console.TryFindConsoleVariable(L"Render.Fluid.SecondaryAdapter", water), "Experiment must not link Bistro water runtime");
        Log::Info("MetaRenderGraph", "U2 PLAN EXECUTOR; primary=0; particle secondary=-1; water runtime absent; single DIRECT execution");
    }

    class MetaApplication final : public App
    {
    public:
        explicit MetaApplication(bool smoke) : App(GetModuleHandleW(nullptr)),_smoke(smoke) {}
        bool _smoke;
        std::vector<Entity> _objects;
        Entity _animated=InvalidEntity;
        bool AddModules() override
        {
            Locator.RegisterModule(std::make_shared<RenderModule>(GetWindow(), &Timer, std::make_unique<MetaRenderPipeline>(_smoke)));
            Locator.RegisterModule(std::make_shared<SceneManagerModule>());
            return BenchmarkEngine::AddModules();
        }
        bool Initialize() override
        {
            if (!App::Initialize()) return false;
            auto scenes = Locator.GetModule<SceneManagerModule>();
            Require(scenes->LoadWorld(Engine::Core::GetExecutableDirectory() / "Configs/SmokeWorld.yaml"), "Cannot create smoke world");
            auto& world = *scenes->GetWorld();
            auto renderer=Locator.GetModule<RenderModule>();
            auto& registry=Engine::Core::AssetManager::GetInstance().Meshes();
            const auto handle=registry.Register({"meta-u4-scene.mesh"});
            Engine::Core::SubMesh part;
            for(const auto position:std::array<Vector3,4>{{{-70,-70,0},{-70,70,0},{70,70,0},{70,-70,0}}}) {
                Engine::Core::Vertex v; v.Position=v.PreviousPosition=position; v.Normal={0,0,-1};
                v.TexCoord={(position.x+70)/140,(position.y+70)/140}; part.Vertices.push_back(v);
            }
            part.Indices={0,1,2,0,2,3}; part.Bounds={{0,0,0},{70,70,1}};
            const auto* mesh=registry.Cache(handle,std::make_unique<Engine::Core::Mesh>(std::vector<Engine::Core::SubMesh>{part},part.Bounds));
            Require(mesh!=nullptr,"Cannot cache geometry scene"); renderer->SubmitMesh(mesh,handle);
            auto* material=renderer->CreateMaterial("Meta U4 opaque");
            Engine::Core::TextureDesc textureDesc; textureDesc.Dimension=Engine::Core::ETextureDimension::Texture2D;
            textureDesc.Type=Engine::Core::ETextureType::Color; textureDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM; textureDesc.Width=textureDesc.Height=1;
            Engine::Core::SubTexture pixel; pixel.Width=pixel.Height=1; pixel.RowPitch=pixel.SlicePitch=4;
            pixel.Data={std::byte{64},std::byte{180},std::byte{230},std::byte{255}};
            const Engine::Core::Texture texture(textureDesc,{pixel});
            material->Diffuse=renderer->CreateTexture("Meta U4 cyan",&texture,Engine::Core::ETextureType::Color);
            for(int i=0;i<3;++i) {
                auto object=world.GetECS().CreateEntity(); auto& transform=object.AddComponent<TransformComponent>();
                transform.Location={float((i-1)*160),0,float(i*30)};
                object.AddComponent<StaticMeshRenderComponent>(handle,std::vector<GDX12Material*>{material});
                _objects.push_back(object.GetId());
            }
            IndirectCommandBuildSystem initialCommands;
            initialCommands.Schedule(GetJobs(),world,0).Complete();
            const auto skinHandle=registry.Register({"meta-u4-skinned.mesh"});
            part.Bones={{"Root",Matrix::Identity}}; part.SkinVertices={{0,1},{1,1},{2,1},{3,1}};
            part.Influences={{0,1},{0,1},{0,1},{0,1}}; part.SourceNodeName="Root";
            Require(registry.Cache(skinHandle,std::make_unique<Engine::Core::Mesh>(std::vector<Engine::Core::SubMesh>{part},part.Bounds,std::vector<Engine::Core::MeshMaterial>{Engine::Core::MeshMaterial{}}))!=nullptr,"Cannot cache skinned mesh");
            auto& sceneRegistry=Engine::Core::AssetManager::GetInstance().Scenes();
            const auto sceneHandle=sceneRegistry.Register("meta-u4-skeleton.scene");
            Engine::Core::SceneNode node; node.Name="Root"; node.LocalBindMatrix=Matrix::Identity; node.Meshes={skinHandle};
            Engine::Core::NodeAnimation track; track.NodeIndex=0; track.Translation={{0,{0,0,0}},{1,{100,100,0}}};
            Engine::Core::AnimationClip clip; clip.Name="Translate"; clip.DurationSeconds=2; clip.Tracks={track};
            Require(sceneRegistry.Cache(sceneHandle,std::make_unique<Engine::Core::SceneAsset>(std::vector<Engine::Core::SceneNode>{node},std::vector<uint32_t>{0},std::vector<Engine::Core::AnimationClip>{clip}))!=nullptr,"Cannot cache animated scene");
            auto animated=world.GetECS().CreateEntity(); _animated=animated.GetId();
            animated.AddComponent<TransformComponent>(Vector3{0,140,20});
            auto& skin=animated.AddComponent<SkeletalMeshRenderComponent>(sceneHandle); skin.SetClip(0); skin.SetMaterials({material});
            auto emitter = world.GetECS().CreateEntity();
            emitter.AddComponent<TransformComponent>();
            GDX12ParticleEmitterSettings particles;
            particles.Seed = 13;
            particles.MaxParticles = 256;
            particles.SharedSimulation = true; // Authoring fixture retained; particle passes migrate in U6.
            particles.Blend = ParticleBlend::Additive;
            particles.EmissionRate = 0;
            auto& component = emitter.AddComponent<ParticleEmitterComponent>();
            component.SetSettings(particles);
            component.Emit(64);
            SetWindowTextW(GetWindow()->GetWindowHandle(), L"MetaRenderGraph U2 - DIRECT plan");
            return true;
        }
        void Smoke()
        {
            auto renderer = Locator.GetModule<RenderModule>();
            ComPtr<ID3D12InfoQueue> info;
            ThrowIfFailed(renderer->GetPrimaryDevice()->GetDevice().As(&info));
            RunDirectExecutionGpuTests(*renderer->GetPrimaryDevice());
            Timer.Reset();
            Timer.Stop(); // Fixed zero dt: reproducible initial burst, no wall-clock simulation.
            const auto frame = [&]
            {
                const UINT frameIndex=UINT(renderer->GetFrameSubmissionId());
                auto& world=*Locator.GetModule<SceneManagerModule>()->GetWorld(); auto& ecs=world.GetECS();
                for(UINT i=0;i<_objects.size();++i) {
                    auto& transform=ecs.Get<TransformComponent>(_objects[i]);
                    transform.Location={float((int(i)-1)*160)+(frameIndex==1 && i==0?80.f:0.f),0,float(i*30)};
                    if(frameIndex==5 && i==0) transform.Location.x=100000;
                    transform.DirtyFlag=true;
                    auto& object=ecs.Get<StaticMeshRenderComponent>(_objects[i]);
                    object.Visible=frameIndex!=3 && frameIndex!=6 && !(frameIndex==2 && i==0);
                    object.DirtyFlag=true;
                }
                IndirectCommandBuildSystem commands; commands.Schedule(GetJobs(),world,0).Complete();
                auto& skin=ecs.Get<SkeletalMeshRenderComponent>(_animated); skin.SetTimeSeconds(frameIndex*0.1);
                auto& animated=ecs.Get<TransformComponent>(_animated); animated.Location.x=frameIndex==3 || frameIndex==6?100000.f:0.f; animated.DirtyFlag=true;
                MSG message{};
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
                { TranslateMessage(&message); DispatchMessageW(&message); }
                Update(Timer);
                Render(Timer);
            };
            for (int i = 0; i < 4; ++i) frame();
            const auto before = renderer->GetFrameSubmissionId();
            Require(SetWindowPos(GetWindow()->GetWindowHandle(), nullptr, 0, 0, 800, 600,
                SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE, "Window resize failed");
            for (int i = 0; i < 4; ++i) frame();
            uint16_t width, height;
            GetWindow()->GetWindowSize(width, height);
            Require(width != 1280 && width > 0 && height > 0, "Resize did not change client extent");
            Require(renderer->GetFrameSubmissionId() == before + 4, "Frames did not submit after resize");
            renderer->DrainFrameWork();
            CheckGeometryGpuEvidence(Engine::Core::GetExecutableDirectory()/"Logs");
            Require(GDX12RenderGraph::GetExecuteCallCount() == 0, "Experiment called legacy graph Execute");
            Log::Info("MetaRenderGraph", "Runtime legacy graph Execute calls=0");
            for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i)
            {
                SIZE_T size = 0;
                ThrowIfFailed(info->GetMessage(i, nullptr, &size));
                std::vector<BYTE> bytes(size);
                auto* message = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
                ThrowIfFailed(info->GetMessage(i, message, &size));
                if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) throw std::runtime_error(message->pDescription);
            }
            const auto root = Engine::Core::GetExecutableDirectory() / "Logs";
            std::ofstream graph(root / "reference.dot");
            renderer->WriteRenderGraph(graph);
            Require(bool(graph), "Cannot write reference graph");
            Log::Info("MetaRenderGraph", "Smoke passed: 8 presented frames, actual window resize to ", width, "x", height,
                "; D3D12 errors=0; fixture seeds=1..8; transform=8*x+7; dt=0");
            // Exercise the normal window close / WM_DESTROY / WM_QUIT loop as well.
            PostMessageW(GetWindow()->GetWindowHandle(), WM_CLOSE, 0, 0);
            Require(Run() == 0, "Window shutdown failed");
        }
    };
}

int RunMetaApplication(bool smoke)
{
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    try
    {
        PrepareStartup();
        if (smoke)
        {
            ComPtr<ID3D12Debug> debug;
            ThrowIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
            debug->EnableDebugLayer();
        }
        {
            MetaApplication app(smoke);
            Require(app.Initialize(), "Application initialization failed");
            if (smoke) app.Smoke();
            else return app.Run();
        }
        Log::Info("MetaRenderGraph", "Smoke shutdown complete");
        Log::Flush();
        return 0;
    }
    catch (const DxException& error) { Log::Error("MetaRenderGraph", error.ToString()); }
    catch (const std::exception& error) { Log::Error("MetaRenderGraph", error.what()); }
    Log::Flush();
    return 1;
}
