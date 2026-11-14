#include "MetaApplication.h"
#include "MetaRenderPipeline.h"
#include "App.Base/ECS/Components/ReflectionProbeComponent.h"
#include "App.Base/App.h"
#include "Common/RuntimePaths.h"
#include "Engine.Core/ConsoleModule.h"
#include "Engine.Core/Logging/Logger.h"
#include "Engine.Core/AssetManager.h"
#include "Components/BistroGrassComponents.h"
#include "Components/BistroInteractionSource.h"
#include "Components/FluidRenderComponent.h"
#include "Engine.UI/UI_test.h"
#include <d3d12sdklayers.h>
#include <fstream>
#include <stdexcept>

void RunDirectExecutionGpuTests(GDX12Device&);
void CheckGeometryGpuEvidence(const std::filesystem::path&);
void CheckShadowGpuEvidence(const std::filesystem::path&);
void CheckGrassGpuEvidence(const std::filesystem::path&);
void CheckReflectionGpuEvidence(const std::filesystem::path&);
void CheckFSRGpuEvidence(const std::string&,unsigned,unsigned);

namespace
{
    const Vector3 FixtureOrigin{1399.6f,100,1369};
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
        Require(console.FindConsoleVariable(L"Render.Fluid.SecondaryAdapter").GetInt() == -1, "Water offload must be disabled");
        Require(!console.FindConsoleVariable(L"Render.Grass.AsyncCompute").GetBool(),"Grass async must be disabled");
        Log::Info("MetaRenderGraph", "U8 SCENE EXECUTOR; primary=0; particle/water secondary=-1; local-only simulation; explicit Meta queues");
    }

    void ValidateMessages(ID3D12InfoQueue& info)
    {
        std::ofstream validation(Engine::Core::GetExecutableDirectory()/"Logs/d3d12-messages.txt");
        Require(bool(validation),"Cannot write D3D12 validation artifact");
        const auto discarded=info.GetNumMessagesDiscardedByMessageCountLimit();
        validation << "stored=" << info.GetNumStoredMessages()
            << " allowed=" << info.GetNumMessagesAllowedByStorageFilter()
            << " denied=" << info.GetNumMessagesDeniedByStorageFilter()
            << " discarded=" << discarded << " limit=" << info.GetMessageCountLimit() << '\n';
        bool validationFailed=discarded!=0;
        SIZE_T filterSize=0;ThrowIfFailed(info.GetStorageFilter(nullptr,&filterSize));
        std::vector<BYTE> filterBytes(filterSize);
        auto* filter=reinterpret_cast<D3D12_INFO_QUEUE_FILTER*>(filterBytes.data());
        ThrowIfFailed(info.GetStorageFilter(filter,&filterSize));
        validation<<"storage_filter_stack="<<info.GetStorageFilterStackSize()<<'\n';
        for(UINT i=0;i<filter->DenyList.NumSeverities;++i) {
            const auto severity=filter->DenyList.pSeverityList[i];
            validation<<"filter_deny_severity="<<severity<<'\n';
            validationFailed|=severity<=D3D12_MESSAGE_SEVERITY_ERROR;
        }
        for(UINT i=0;i<filter->DenyList.NumIDs;++i) validation<<"filter_deny_id="<<filter->DenyList.pIDList[i]<<'\n';
        for(UINT i=0;i<filter->DenyList.NumCategories;++i) validation<<"filter_deny_category="<<filter->DenyList.pCategoryList[i]<<'\n';
        UINT64 severityCounts[5]{};
        for (UINT64 i = 0; i < info.GetNumStoredMessages(); ++i)
        {
            SIZE_T size = 0;
            ThrowIfFailed(info.GetMessage(i, nullptr, &size));
            std::vector<BYTE> bytes(size);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
            ThrowIfFailed(info.GetMessage(i, message, &size));
            validation << "severity=" << message->Severity << " id=" << message->ID << " " << message->pDescription << '\n';
            ++severityCounts[message->Severity];
            validationFailed |= message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR;
    }
    for(UINT i=0;i<5;++i) validation << "severity_count " << i << " " << severityCounts[i] << '\n';
    validation.close();
    Require(!validationFailed,"D3D12 validation failed; see Logs/d3d12-messages.txt");
    }

    class MetaApplication final : public App
    {
    public:
        explicit MetaApplication(bool smoke) : App(GetModuleHandleW(nullptr)),_smoke(smoke) {}
        bool _smoke;
        std::vector<Entity> _objects;
        Entity _animated=InvalidEntity;
        Entity _animatedPeer=InvalidEntity;
        Entity _grass=InvalidEntity,_wind=InvalidEntity,_interaction=InvalidEntity;
        Entity _fluid=InvalidEntity;
        MetaRenderPipeline* _pipeline=nullptr;
        bool AddModules() override
        {
            auto pipeline=std::make_unique<MetaRenderPipeline>(_smoke);_pipeline=pipeline.get();
            Locator.RegisterModule(std::make_shared<RenderModule>(GetWindow(), &Timer, std::move(pipeline)));
            Locator.RegisterModule(std::make_shared<SceneManagerModule>());
            return BenchmarkEngine::AddModules();
        }
        bool Initialize() override
        {
            if (!App::Initialize()) return false;
            auto scenes = Locator.GetModule<SceneManagerModule>();
            Require(scenes->LoadWorld(Engine::Core::GetExecutableDirectory()/"Configs/SmokeWorld.yaml"), "Cannot create smoke world");
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
                transform.Location=FixtureOrigin+Vector3{float((i-1)*160),0,float(i*30)};
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
            animated.AddComponent<TransformComponent>(FixtureOrigin+Vector3{0,140,20});
            auto& skin=animated.AddComponent<SkeletalMeshRenderComponent>(sceneHandle); skin.SetClip(0); skin.SetMaterials({material});
            auto peer=world.GetECS().CreateEntity();_animatedPeer=peer.GetId();peer.AddComponent<TransformComponent>(FixtureOrigin+Vector3{160,140,20});
            auto& peerSkin=peer.AddComponent<SkeletalMeshRenderComponent>(sceneHandle);peerSkin.SetClip(0);peerSkin.SetTimeSeconds(.25);peerSkin.SetMaterials({material});
            for(UINT i=0;i<3;++i) {
                auto light=world.GetECS().CreateEntity(); light.AddComponent<TransformComponent>(FixtureOrigin+Vector3{float(i*200),50,-200});
                auto& settings=light.AddComponent<LightComponent>(); settings.Type=i==0?LightType::Point:i==1?LightType::Spot:LightType::Directional;
                if(i==2) { auto& transform=light.GetComponent<TransformComponent>(); transform.Rotation={40,30,0}; }
                settings.Range=1000; settings.ShadowUpdate=ShadowUpdateMode::EveryFrame;
            }
            auto probe=world.GetECS().CreateEntity();probe.AddComponent<TransformComponent>(FixtureOrigin+Vector3{0,30,-100});
            auto& reflection=probe.AddComponent<ReflectionProbeComponent>();reflection.Resolution=32;reflection.FacesPerFrame=6;reflection.PrefilterSamples=16;reflection.Implementation=1;reflection.UpdateInterval=2;reflection.Extents=Vector3(1000);
            auto wind=world.GetECS().CreateEntity(); _wind=wind.GetId(); wind.AddComponent<TransformComponent>(FixtureOrigin);
            auto& field=wind.AddComponent<WindFieldComponent>(); field.GridResolution=16; field.PressureIterations=4; field.MaxSubsteps=1;
            auto grass=world.GetECS().CreateEntity(); _grass=grass.GetId(); grass.AddComponent<TransformComponent>(FixtureOrigin+Vector3{0,-76.7f,0});
            auto& patch=grass.AddComponent<GrassRenderComponent>(); patch.Material=material; patch.WindEntity=_wind;
            patch.ExtentsCm={160,160}; patch.MaxBlades=256; patch.Segments=3; patch.HeightCm=90; patch.WidthCm=5; patch.Seed=13;
            for(int z=0;z<16;++z)for(int x=0;x<16;++x)patch.RootsCm.push_back({float(x*20-150),0,float(z*20-150)});
            world.GetECS().RegisterComponent<BistroInteractionSource>();
            auto interaction=world.GetECS().CreateEntity();_interaction=interaction.GetId();interaction.AddComponent<BistroInteractionSource>();
            auto emitter = world.GetECS().CreateEntity();
            emitter.AddComponent<TransformComponent>().Location=FixtureOrigin+Vector3{0,0,-180};
            GDX12ParticleEmitterSettings particles;
            particles.Seed = 13;
            particles.MaxParticles = 256;
            particles.SharedSimulation = true; // Authoring fixture retained; particle passes migrate in U6.
            particles.Blend = ParticleBlend::Additive;
            particles.EmissionRate = 0;
            auto& component = emitter.AddComponent<ParticleEmitterComponent>();
            component.SetSettings(particles);
            component.Emit(64);
            for(UINT i=0;i<4;++i) {
                auto extra=world.GetECS().CreateEntity();
                extra.AddComponent<TransformComponent>().Location=FixtureOrigin+Vector3{float(i*12),0,float(int(i)*80-180)};
                auto settings=particles;
                settings.Seed=12345+i;
                settings.DepthCollision=i%2!=0;
                settings.Blend=i<2?ParticleBlend::SortedAlpha:ParticleBlend::WeightedAlpha;
                settings.ColorStart=settings.ColorEnd=Vector4(i%2?0.f:1.f,i%2?1.f:0.f,.4f,.4f);
                auto& source=extra.AddComponent<ParticleEmitterComponent>(); source.SetSettings(settings); source.Emit(4);
            }
            auto fluid=world.GetECS().CreateEntity(); _fluid=fluid.GetId();
            fluid.AddComponent<TransformComponent>().Location=FixtureOrigin+Vector3{0,0,-90};
            auto& water=fluid.AddComponent<FluidRenderComponent>();
            GDX12FluidSettings waterSettings;
            waterSettings.ReferenceModel=true;
            waterSettings.ParticleRadiusCm=9; waterSettings.SmoothingRadiusCm=24;
            waterSettings.FilterIterations=1; waterSettings.FilterRadiusPixels=2;
            water.SetSettings(waterSettings);
            std::vector<GDX12FluidParticle> seeds;
            for(int z=0;z<4;++z) for(int y=0;y<4;++y) for(int x=0;x<4;++x)
                seeds.push_back({Vector3{float(x*12-18),float(y*12-18),float(z*12-18)},Vector3::Zero});
            water.SetParticles(std::move(seeds));
            SetWindowTextW(GetWindow()->GetWindowHandle(), L"MetaRenderGraph U5 - lighting and reflections");
            return true;
        }
        void FSRChecks()
        {
            Timer.Reset();
            Engine::UI::SuspendSettings(true);
            struct RestoreSettings { ~RestoreSettings(){Engine::UI::SuspendSettings(false);} } restoreSettings;
            auto renderer=Locator.GetModule<RenderModule>();
            auto& world=*Locator.GetModule<SceneManagerModule>()->GetWorld();
            ComPtr<ID3D12InfoQueue> info;ThrowIfFailed(renderer->GetPrimaryDevice()->GetDevice().As(&info));
            _pipeline->SkipIsolatedFixtures();_pipeline->EnablePostVerification();
            const auto set=[](const wchar_t* name,int value) {
                Require(ConsoleModule{}.FindConsoleVariable(name).SetFromString(std::to_string(value)),"FSR check CVar rejected");
            };
            set(L"Render.FrameGeneration.Provider",0);set(L"Render.Upscaler.Provider",2);
            set(L"Render.DOF.Enabled",0);set(L"Render.MotionBlur.Enabled",0);
            const auto root=Engine::Core::GetExecutableDirectory()/"Logs";
            std::ofstream results(root/"fsr-checks.csv");
            Require(bool(results),"Cannot write FSR checks");
            results<<"phase,frame,dt,quality,requested,effective,generation,history,render_width,render_height,output_width,output_height,allocation_bytes,allocation_count\n";
            const auto frame=[&](const char* phase) {
                MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {TranslateMessage(&message);DispatchMessageW(&message);}
                const auto index=renderer->GetFrameSubmissionId();
                Timer.Tick(1./60.,double(index)/60.);
                auto& transform=world.GetECS().Get<TransformComponent>(_fluid);
                transform.Location.x=FixtureOrigin.x+float(index*2);transform.DirtyFlag=true;
                Update(Timer);Render(Timer);
                const auto status=_pipeline->ReconstructionStatus();
                Require(status.Requested.Provider==GDX12UpscalerProvider::FSR && status.Effective==GDX12UpscalerProvider::FSR,"FSR did not execute as requested");
                Require(status.FallbackReason.empty(),"FSR silently fell back");
                auto* common=renderer->GetRenderPipelineCommonData();
                CheckFSRGpuEvidence((root/("geometry-"+std::to_string(index))).string(),common->WindowWidth,common->WindowHeight);
                const auto memory=renderer->GetMemoryStatistics();
                results<<phase<<','<<index<<','<<Timer.DeltaTime()<<','<<unsigned(status.Requested.Quality)<<",2,2,"<<status.Generation<<','<<common->HistoryRevision<<','<<common->DownscaledWidth<<','<<common->DownscaledHeight<<','<<common->WindowWidth<<','<<common->WindowHeight<<','<<memory.AllocationBytes<<','<<memory.AllocationCount<<'\n';results.flush();
            };
            try {
                for(int i=0;i<16;++i) frame("default-stability");
                ValidateMessages(*info.Get());
                for(int quality=1;quality<=5;++quality) {
                    set(L"Render.Upscaler.Quality",quality);
                    for(int i=0;i<3;++i) frame("quality-change");
                    ValidateMessages(*info.Get());
                    const auto* common=renderer->GetRenderPipelineCommonData();
                    Require(quality==1 ? common->DownscaledWidth==common->WindowWidth : common->DownscaledWidth<common->WindowWidth,"FSR preset returned the wrong render scale");
                }
                set(L"Render.Upscaler.Quality",2);frame("quality-reference");
                auto* common=renderer->GetRenderPipelineCommonData();
                const auto history=common->HistoryRevision;
                world.GetECS().Get<CameraComponent>(world.ActiveCamera).InvalidateHistory();
                frame("camera-cut");Require(common->HistoryRevision>history,"FSR camera cut did not reset history");
                const auto generation=_pipeline->ReconstructionStatus().Generation;
                set(L"Render.Reconstruction.Restart",ConsoleModule{}.FindConsoleVariable(L"Render.Reconstruction.Restart").GetInt()+1);
                for(int i=0;i<3;++i) frame("restart");
                Require(_pipeline->ReconstructionStatus().Generation>generation,"FSR restart did not recreate its context");
                Require(SetWindowPos(GetWindow()->GetWindowHandle(),nullptr,0,0,913,647,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE)!=FALSE,"FSR resize failed");
                for(int i=0;i<4;++i) frame("odd-resize");
                Require(common->WindowWidth!=1280 && common->WindowHeight!=800,"FSR output did not resize");
                renderer->DrainFrameWork();ValidateMessages(*info.Get());
                Require(GDX12RenderGraph::GetExecuteCallCount()==0,"FSR checks called legacy graph Execute");
                Log::Info("MetaRenderGraph","PASS FSR checks: default stability, NativeAA/Quality/Balanced/Performance/UltraPerformance, HDR readbacks, camera cut, restart, odd resize; FG off; layer errors=0");
            } catch(...) {try {ValidateMessages(*info.Get());} catch(...) {} throw;}
            PostMessageW(GetWindow()->GetWindowHandle(),WM_CLOSE,0,0);Require(Run()==0,"FSR shutdown failed");
        }
        void TechniqueMatrix(unsigned firstCase)
        {
            Timer.Reset();Timer.Stop();
            auto renderer=Locator.GetModule<RenderModule>();
            ComPtr<ID3D12InfoQueue> info;ThrowIfFailed(renderer->GetPrimaryDevice()->GetDevice().As(&info));
            _pipeline->SkipIsolatedFixtures();
            const auto set=[](const wchar_t* name,int value) {
                Require(ConsoleModule{}.FindConsoleVariable(name).SetFromString(std::to_string(value)),"Matrix CVar rejected");
            };
            std::ofstream matrix(Engine::Core::GetExecutableDirectory()/"Logs/ao-ssr-matrix.csv");
            Require(bool(matrix),"Cannot write technique matrix");
            matrix<<"first_frame,frame_count,ao_enabled,ao_implementation,ao_resolution,ao_execution_point,ssr_quality,result\n";
            const auto frames=[&](int implementation,int resolution,int point,int ssr) {
                const auto first=renderer->GetFrameSubmissionId();
                Log::Info("MetaRenderGraph","MATRIX begin frame=",first," AO=",implementation," resolution=",resolution," point=",point," SSR quality=",ssr);
                for(int i=0;i<2;++i) {
                    MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {TranslateMessage(&message);DispatchMessageW(&message);}
                    Update(Timer);Render(Timer);
                }
                renderer->DrainFrameWork();ValidateMessages(*info.Get());
                matrix<<first<<",2,"<<(implementation>=0)<<','<<implementation<<','<<resolution<<','<<point<<','<<ssr<<",PASS\n";
                matrix.flush();
            };
            try {
                set(L"Render.SSR.Enabled",0);set(L"Render.AO.Enabled",1);
                unsigned scenario=0;
                for(int implementation=0;implementation<4;++implementation)
                    for(int resolution=0;resolution<2;++resolution)
                        for(int point=0;point<2;++point) {
                            if(scenario++<firstCase) continue;
                            set(L"Render.AO.Implementation",implementation);set(L"Render.AO.Resolution",resolution);set(L"Render.AO.ExecutionPoint",point);
                            frames(implementation,resolution,point,0);
                        }
                set(L"Render.AO.Enabled",0);set(L"Render.SSR.Enabled",1);
                for(int quality=1;quality<=4;++quality) {if(scenario++<firstCase) continue;set(L"Render.SSR.Quality",quality);frames(-1,0,0,quality);}
                set(L"Render.SSR.Enabled",0);
                Require(GDX12RenderGraph::GetExecuteCallCount()==0,"Technique matrix called legacy graph Execute");
                Log::Info("MetaRenderGraph","PASS AO/SSR matrix suffix: first case=",firstCase," completed cases=",20-firstCase," errors=0; legacy Execute=0");
            } catch(...) {try {ValidateMessages(*info.Get());} catch(...) {} throw;}
            PostMessageW(GetWindow()->GetWindowHandle(),WM_CLOSE,0,0);
            Require(Run()==0,"Matrix shutdown failed");
        }
        void Smoke(bool focused, bool sceneOnly)
        {
            auto renderer = Locator.GetModule<RenderModule>();
            ComPtr<ID3D12InfoQueue> info;
            ThrowIfFailed(renderer->GetPrimaryDevice()->GetDevice().As(&info));
            struct MessagesOnFailure {
                ID3D12InfoQueue* Info;
                int Exceptions=std::uncaught_exceptions();
                ~MessagesOnFailure() {
                    if(std::uncaught_exceptions()>Exceptions)
                        try { ValidateMessages(*Info); }
                        catch(const std::exception& error) { Log::Error("MetaRenderGraph",error.what()); }
                        catch(...) { Log::Error("MetaRenderGraph","InfoQueue dump failed during unwinding"); }
                }
            } messagesOnFailure{info.Get()};
            if (sceneOnly) _pipeline->SkipIsolatedFixtures();
            else RunDirectExecutionGpuTests(*renderer->GetPrimaryDevice());
            if (focused)
            {
                ValidateMessages(*info.Get());
                Log::Info("MetaRenderGraph","PASS focused executor fixtures; D3D12 errors=0");
                PostMessageW(GetWindow()->GetWindowHandle(), WM_CLOSE, 0, 0);
                Require(Run()==0,"Focused fixture shutdown failed");
                return;
            }
            Timer.Reset();
            Timer.Stop(); // Fixed zero dt: reproducible initial burst, no wall-clock simulation.
            const auto frame = [&]
            {
                const UINT frameIndex=UINT(renderer->GetFrameSubmissionId());
                auto& world=*Locator.GetModule<SceneManagerModule>()->GetWorld(); auto& ecs=world.GetECS();
                auto& water=ecs.Get<FluidRenderComponent>(_fluid);
                auto waterSettings=water.GetSettings();
                const bool renderWater=frameIndex!=2 && frameIndex!=6;
                if(waterSettings.RenderEnabled!=renderWater) {waterSettings.RenderEnabled=renderWater;water.SetSettings(waterSettings);}
                if(frameIndex==4) water.Reset();
                auto& container=ecs.Get<TransformComponent>(_fluid);
                container.Location=FixtureOrigin+Vector3{float(frameIndex*8),0,-90}; container.DirtyFlag=true;
                if(frameIndex<8) {
                for(UINT i=0;i<_objects.size();++i) {
                    auto& transform=ecs.Get<TransformComponent>(_objects[i]);
                    transform.Location=FixtureOrigin+Vector3{float((int(i)-1)*160)+(frameIndex==1 && i==0?80.f:0.f),0,float(i*30)};
                    if(frameIndex==5 && i==0) transform.Location.x=100000;
                    transform.DirtyFlag=true;
                    auto& object=ecs.Get<StaticMeshRenderComponent>(_objects[i]);
                    object.Visible=frameIndex!=3 && frameIndex!=6 && !(frameIndex==2 && i==0);
                    object.DirtyFlag=true;
                }
                IndirectCommandBuildSystem commands; commands.Schedule(GetJobs(),world,0).Complete();
                auto& skin=ecs.Get<SkeletalMeshRenderComponent>(_animated); skin.SetTimeSeconds(frameIndex*0.1);
                auto& animated=ecs.Get<TransformComponent>(_animated); animated.Location.x=FixtureOrigin.x+(frameIndex==3 || frameIndex==6?100000.f:0.f); animated.DirtyFlag=true;
                auto& peerSkin=ecs.Get<SkeletalMeshRenderComponent>(_animatedPeer);peerSkin.SetTimeSeconds(frameIndex*.1+.25);
                auto& peer=ecs.Get<TransformComponent>(_animatedPeer);peer.Location.x=FixtureOrigin.x+(frameIndex==3 || frameIndex==6?100000.f:160.f);peer.DirtyFlag=true;
                auto& patch=ecs.Get<GrassRenderComponent>(_grass);patch.Enabled=frameIndex!=3 && frameIndex!=6;
                auto& field=ecs.Get<WindFieldComponent>(_wind);field.Paused=frameIndex==2;
                if(frameIndex==4){++field.ResetRevision;++patch.ResetRevision;patch.Implementation=GDX12GrassImplementation::CrossedRibbons;}
                if(frameIndex==7)field.Implementation=GDX12WindImplementation::Analytic;
                auto& grassTransform=ecs.Get<TransformComponent>(_grass);grassTransform.Location.x=FixtureOrigin.x+(frameIndex==5?40:0);grassTransform.DirtyFlag=true;
                auto& source=ecs.Get<BistroInteractionSource>(_interaction);
                source.PositionRadius={FixtureOrigin.x+float(frameIndex*20),23.3f,FixtureOrigin.z,45};
                source.PreviousHeight={FixtureOrigin.x+float(frameIndex?frameIndex*20-20:0),23.3f,FixtureOrigin.z,190};
                }
                MSG message{};
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
                { TranslateMessage(&message); DispatchMessageW(&message); }
                Update(Timer);
                Render(Timer);
            };
            for (int i = 0; i < 4; ++i) frame();
            const auto initialMemory=renderer->GetMemoryStatistics();
            Log::Info("MetaRenderGraph","U4 resource allocation before resize bytes=",initialMemory.AllocationBytes," count=",initialMemory.AllocationCount," block bytes=",initialMemory.BlockBytes);
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
            CheckShadowGpuEvidence(Engine::Core::GetExecutableDirectory()/"Logs");
            CheckGrassGpuEvidence(Engine::Core::GetExecutableDirectory()/"Logs");
            CheckReflectionGpuEvidence(Engine::Core::GetExecutableDirectory()/"Logs");
            _pipeline->SetVerification(false);
            for(int i=0;i<4;++i)frame();
            _pipeline->ValidateQueued(*renderer->GetPrimaryDevice(),*renderer->GetRenderPipelineCommonData());
            const auto finalMemory=renderer->GetMemoryStatistics();
            Log::Info("MetaRenderGraph","U4 resource allocation after queued frames bytes=",finalMemory.AllocationBytes," count=",finalMemory.AllocationCount," block bytes=",finalMemory.BlockBytes);
            Require(GDX12RenderGraph::GetExecuteCallCount() == 0, "Experiment called legacy graph Execute");
            Log::Info("MetaRenderGraph", "Runtime legacy graph Execute calls=0");
            ValidateMessages(*info.Get());
            const auto root = Engine::Core::GetExecutableDirectory() / "Logs";
            std::ofstream graph(root / "reference.dot");
            renderer->WriteRenderGraph(graph);
            Require(bool(graph), "Cannot write reference graph");
            Log::Info("MetaRenderGraph", "Smoke passed: 12 presented frames, actual window resize to ", width, "x", height,
                "; D3D12 errors=0; 8 scene serial/parallel pairs + 4 queued frames; animation inputs fixed; host dt=0; compared wind/interaction dt=1/60");
            // Exercise the normal window close / WM_DESTROY / WM_QUIT loop as well.
            PostMessageW(GetWindow()->GetWindowHandle(), WM_CLOSE, 0, 0);
            Require(Run() == 0, "Window shutdown failed");
        }
    };
}

int RunMetaApplication(bool smoke, bool focused, bool gbv, bool sceneOnly, MetaValidationMatrix matrix, unsigned firstCase)
{
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    try
    {
        Require(!gbv || focused,"GBV requires focused executor fixtures");
        PrepareStartup();
        if (smoke)
        {
            ComPtr<ID3D12Debug> debug;
            ThrowIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
            debug->EnableDebugLayer();
            if (gbv)
            {
                ComPtr<ID3D12Debug1> validation;
                ThrowIfFailed(debug.As(&validation));
                validation->SetEnableGPUBasedValidation(TRUE);
            }
        }
        {
            MetaApplication app(smoke);
            Require(app.Initialize(), "Application initialization failed");
            if(matrix==MetaValidationMatrix::AO_SSR) app.TechniqueMatrix(firstCase);
            else if(matrix==MetaValidationMatrix::FSR) app.FSRChecks();
            else if (smoke) app.Smoke(focused,sceneOnly);
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
