#include "MetaApplication.h"
#include "MetaRenderPipeline.h"
#include "App.Base/ECS/Components/ReflectionProbeComponent.h"
#include "App.Base/App.h"
#include "Common/RuntimePaths.h"
#include "Engine.Core/ConsoleModule.h"
#include "Engine.Core/Logging/Logger.h"
#include "Engine.Core/AssetManager.h"
#include "Components/BistroGrassComponents.h"
#include "BistroGrass.h"
#include "BistroScene.h"
#include "BistroBounds.h"
#include "BistroCharacters.h"
#include "BistroWater.h"
#include "BistroReflections.h"
#include "Components/BistroInteractionSource.h"
#include "Components/FluidRenderComponent.h"
#include <d3d12sdklayers.h>
#include <fstream>
#include <stdexcept>
#include <algorithm>
#include <cmath>
#include <WindowsX.h>
#include "Engine.UI/UI_test.h"

void RunDirectExecutionGpuTests(GDX12Device&);
void CheckGeometryGpuEvidence(const std::filesystem::path&);
void CheckShadowGpuEvidence(const std::filesystem::path&);
void CheckGrassGpuEvidence(const std::filesystem::path&);
void CheckReflectionGpuEvidence(const std::filesystem::path&);

namespace
{
    AutoConsoleVariable<int> MetaSkySource(L"Meta.Sky.Source", 2,
        L"Ordinary Bistro sky: 0 Off, 1 original SkyBox, 2 SkyAtmosphere",
        EConsoleVariableFlags::Default, [](int value) { return value >= 0 && value <= 2; });
    AutoConsoleVariable<bool> MetaSunCycle(L"Meta.Sky.SunCycle.Enabled", true,
        L"Move the original Sun through a repeating daylight arc; disable for manual rotation");
    AutoConsoleVariable<float> MetaSunCyclePeriod(L"Meta.Sky.SunCycle.PeriodSeconds", 120.f,
        L"Seconds from sunrise through zenith to sunset; repeats without night",
        EConsoleVariableFlags::Default, [](float value) { return std::isfinite(value) && value > 0; });
    const Vector3 FixtureOrigin{1399.6f,100,1369};
    void Require(bool value, const char* message)
    {
        if (!value) throw std::runtime_error(message);
    }

    // Runs before BenchmarkEngine's constructor reads and locks startup CVars.
    // Both files belong exclusively to this executable's output directory.
    void PrepareStartup(bool smoke)
    {
        const auto root = Engine::Core::GetExecutableDirectory();
        std::filesystem::create_directories(root / "Config");
        std::filesystem::copy_file(root / "Configs/Startup.ini", root / "Config/Engine.ini",
            std::filesystem::copy_options::overwrite_existing);
        if (smoke)
        {
            std::ofstream config(root / "Config/Engine.ini", std::ios::app);
            config << "\nRender.Shadows.Sun.Resolution = 512\nRender.Shadows.Local.Resolution = 128\n";
            Require(bool(config), "Cannot configure procedural diagnostics");
        }
        // Initialize the registry now so the invariant is checked before any device is opened.
        ConsoleModule console;
        console.Initialize();
        Require(console.FindConsoleVariable(L"Render.PrimaryAdapter").GetInt() == 0, "Unexpected primary adapter");
        Require(console.FindConsoleVariable(L"Render.Particles.SecondaryAdapter").GetInt() == -1, "Particle offload must be disabled");
        Require(!console.FindConsoleVariable(L"Render.Particles.AsyncCompute").GetBool(), "Particle async must be disabled");
        Require(!console.FindConsoleVariable(L"Render.AO.AsyncCompute").GetBool(), "AO async must be disabled");
        Require(console.FindConsoleVariable(L"Render.Fluid.SecondaryAdapter").GetInt() == -1, "Water offload must be disabled");
        Require(!console.FindConsoleVariable(L"Render.Grass.AsyncCompute").GetBool(),"Grass async must be disabled");
        Log::Info("MetaRenderGraph", "SCENE EXECUTOR; primary=0; particle/water secondary=-1; Meta.Async.Queues=",
            console.FindConsoleVariable(L"Meta.Async.Queues").GetBool());
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
        Entity _skyEntity=InvalidEntity;
        double _sunCyclePhase = 0;
        MetaRenderPipeline* _pipeline=nullptr;
        BistroGrass::Scene _bistroGrass;
        BistroCharacters::Scene _bistroCharacters;
        BistroWater::Scene _bistroWater;
        BistroReflections::Scene _bistroReflections{1, true};
        std::unique_ptr<BistroBounds::Scene> _bistroBounds;
        void Update(const GameTimer& timer) override
        {
            if (!_smoke)
            {
                auto& world = *Locator.GetModule<SceneManagerModule>()->GetWorld();
                _bistroReflections.Update(world, timer.DeltaTime());
                const float sceneDelta = world.IsPaused() ? 0.f : timer.DeltaTime() * world.GetTimeScale();
                UpdateSky(world, sceneDelta);
                _bistroWater.Update(world, timer.DeltaTime());
                _bistroCharacters.Update(world, timer.DeltaTime(), GetJobs());
                _bistroGrass.Update(world, timer.DeltaTime());
                _bistroBounds->UpdateEmitters(world);
                BistroParticles::Update(world);
            }
            App::Update(timer);
            if (!_smoke)
                _bistroReflections.DrawBounds(*Locator.GetModule<SceneManagerModule>()->GetWorld(),
                    *Locator.GetModule<RenderModule>());
        }
        bool AddModules() override
        {
            auto pipeline=std::make_unique<MetaRenderPipeline>(_smoke, !_smoke);_pipeline=pipeline.get();
            Locator.RegisterModule(std::make_shared<RenderModule>(GetWindow(), &Timer, std::move(pipeline)));
            if (_smoke) Locator.RegisterModule(std::make_shared<SceneManagerModule>());
            else
            {
                const auto path = std::filesystem::path(ASSETS_FOLDER) / "Scenes/Amazon Lumberyard Bistro";
                for (const auto* part : {"Exterior/exterior.obj", "Interior/interior.obj"})
                    Require(std::filesystem::is_regular_file(path / part), "Missing Bistro scene part");
                Locator.RegisterModule(std::make_shared<SceneManagerModule>(path));
            }
            return BenchmarkEngine::AddModules();
        }
        bool Initialize() override
        {
            if (!App::Initialize()) return false;
            auto scenes = Locator.GetModule<SceneManagerModule>();
            if (!_smoke)
            {
                auto& world = *scenes->GetWorld();
                world.SetPaused(false);
                _skyEntity = world.GetSkyEntity();
                Require(world.GetECS().Has<SkyBoxComponent>(_skyEntity) &&
                    world.GetECS().Has<LightComponent>(_skyEntity), "Bistro sky must use the existing Sun");
                if (!world.GetECS().Has<SkyAtmosphereComponent>(_skyEntity))
                {
                    world.GetECS().Add<SkyAtmosphereComponent>(_skyEntity);
                    auto& atmosphere = world.GetECS().Get<SkyAtmosphereComponent>(_skyEntity);
                    auto settings = atmosphere.GetSettings();
                    settings.SunAngularRadiusRadians *= 2.5f;
                    atmosphere.SetSettings(settings);
                }
                _bistroReflections.Update(world, 0);
                UpdateSky(world);
                _bistroGrass.Update(world, 0);
                CreateBistroParticles(world);
                _bistroWater.Update(world, 0);
                world.GetECS().ForEach<FluidRenderComponent>([](Entity, const FluidRenderComponent& fluid) {
                    Log::Info("MetaRenderGraph", "Bistro water target=", BistroWater::ParticleCount.GetValue(),
                        "; seed particles=", fluid.GetParticles() ? fluid.GetParticles()->size() : 0);
                });
                _bistroCharacters.Update(world, 0, GetJobs());
                _bistroBounds = std::make_unique<BistroBounds::Scene>(world);
                _bistroBounds->UpdateEmitters(world);
                BistroParticles::Update(world);
                auto& camera = world.GetECS().Get<TransformComponent>(world.ActiveCamera);
                camera.Location = {400, 230, -370};
                camera.Rotation = {8, 40, 0};
                camera.DirtyFlag = true;
                world.GetECS().Get<CameraComponent>(world.ActiveCamera).InvalidateHistory();
                IndirectCommandBuildSystem commands;
                commands.Schedule(GetJobs(), world, 0).Complete();
                Log::Info("MetaRenderGraph", "Loaded Bistro interior/exterior; fixed-step interior physics enabled; sun and local shadows");
                return true;
            }
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
            if (_smoke)
            {
                auto emitter = world.GetECS().CreateEntity();
                emitter.AddComponent<TransformComponent>();
                GDX12ParticleEmitterSettings particles;
                particles.Seed = 13; particles.MaxParticles = 256;
                particles.SharedSimulation = true; particles.Blend = ParticleBlend::Additive;
                particles.EmissionRate = 0;
                auto& component = emitter.AddComponent<ParticleEmitterComponent>();
                component.SetSettings(particles); component.Emit(64);
            }
            else
            {
                // Ordinary scene content stays visible and active without a verification harness.
                auto& source = world.GetECS().Get<BistroInteractionSource>(_interaction);
                source.PositionRadius = {FixtureOrigin.x + 70, FixtureOrigin.y - 75, FixtureOrigin.z - 100, 65};
                source.PreviousHeight = {FixtureOrigin.x + 70, FixtureOrigin.y - 75, FixtureOrigin.z - 100, 140};
                for (UINT i = 0; i < 4; ++i)
                {
                    auto emitter = world.GetECS().CreateEntity();
                    emitter.AddComponent<TransformComponent>(FixtureOrigin + Vector3{float(int(i) * 110 - 165), -40, -100});
                    GDX12ParticleEmitterSettings particles;
                    particles.Seed = 13 + i; particles.MaxParticles = 256;
                    particles.EmissionRate = 45; particles.LifetimeMin = 2; particles.LifetimeMax = 3;
                    particles.SpeedMin = 45; particles.SpeedMax = 75; particles.Acceleration = {0, -40, 0};
                    particles.SizeStart = 7; particles.SizeEnd = 2;
                    particles.SharedSimulation = i >= 2;
                    particles.DepthCollision = (i & 1) != 0;
                    particles.InteractionEnabled = i == 2;
                    particles.Blend = i < 2 ? ParticleBlend::SortedAlpha : ParticleBlend::WeightedAlpha;
                    particles.ColorStart = i < 2 ? Vector4(1, .3f, .1f, .8f) : Vector4(.1f, .7f, 1, .8f);
                    auto& component = emitter.AddComponent<ParticleEmitterComponent>();
                    component.SetSettings(particles); component.Emit(32);
                }
                auto water = world.GetECS().CreateEntity();
                water.AddComponent<TransformComponent>(FixtureOrigin + Vector3{0, -50, -170});
                auto& fluid = water.AddComponent<FluidRenderComponent>();
                GDX12FluidSettings settings;
                settings.ContainerHalfExtentCm = {55, 20, 35};
                settings.ParticleRadiusCm = 2; settings.SmoothingRadiusCm = 6;
                settings.MaxSubsteps = 3; settings.FilterRadiusPixels = 4;
                settings.AccelerationCmPerSecond2 = {0, -120, 0};
                fluid.SetSettings(settings);
                std::vector<GDX12FluidParticle> seed;
                for (int z = 0; z < 12; ++z)
                    for (int y = 0; y < 6; ++y)
                        for (int x = 0; x < 16; ++x)
                            seed.push_back({Vector3{float(x * 4 - 30), float(y * 3 - 8), float(z * 4 - 22)},
                                Vector3{x < 8 ? -18.f : 18.f, 0, 0}});
                fluid.SetParticles(std::move(seed));
            }
            SetWindowTextW(GetWindow()->GetWindowHandle(), L"MetaRenderGraph - procedural water and particle scene");
            return true;
        }
        void UpdateSky(World& world, float deltaTime = 0)
        {
            const auto source = static_cast<SkySource>(MetaSkySource.GetValue());
            if (source == SkySource::SkyAtmosphere)
            {
                auto& transform = world.GetECS().Get<TransformComponent>(_skyEntity);
                bool wrapped = false;
                if (MetaSunCycle.GetValue())
                {
                    const double nextPhase = _sunCyclePhase + double(deltaTime) / MetaSunCyclePeriod.GetValue();
                    wrapped = nextPhase >= 1.0;
                    _sunCyclePhase = std::fmod(nextPhase, 1.0);
                    transform.Rotation.x = float(_sunCyclePhase * 180.0);
                    transform.Rotation.z = 0;
                }
                const auto rotation = Matrix::CreateFromYawPitchRoll(XMConvertToRadians(transform.Rotation.y),
                    XMConvertToRadians(transform.Rotation.x), XMConvertToRadians(transform.Rotation.z));
                auto& atmosphere = world.GetECS().Get<SkyAtmosphereComponent>(_skyEntity);
                auto settings = atmosphere.GetSettings();
                const Vector3 directionToSun = rotation.Forward();
                if (settings.DirectionToSun != directionToSun)
                {
                    settings.DirectionToSun = directionToSun;
                    if (wrapped) atmosphere.SetSettings(settings);
                    else atmosphere.UpdateSunDirection(directionToSun);
                }
            }
            if (world.GetSkySource() == source &&
                (source == SkySource::Off || world.GetSkyEntity() == _skyEntity)) return;
            world.SetSkySource(source, _skyEntity);
            Log::Info("MetaRenderGraph", "Ordinary sky source=", static_cast<unsigned>(source),
                "; atmosphere quality=", GetSkyAtmosphereQuality(), "; daylight cycle=", MetaSunCycle.GetValue(),
                "; daylight seconds=", MetaSunCyclePeriod.GetValue());
        }
        POINT _lastMousePos{};
        float _cameraSpeed = 500.f;
        void OnMouseDown(WPARAM, int x, int y) override
        {
            _lastMousePos = { x, y };
            if (Engine::UI::WantsMouseInput()) return;
            SetCapture(GetWindow()->GetWindowHandle());
        }

        void OnMouseUp(WPARAM, int, int) override
        {
            ReleaseCapture();
        }

        void OnMouseMove(WPARAM buttons, int x, int y) override
        {
            if (!_smoke && !Engine::UI::WantsMouseInput() && (buttons & MK_RBUTTON))
            {
                auto* world = Locator.GetModule<SceneManagerModule>()->GetWorld();
                auto& camera = world->GetECS().Get<TransformComponent>(world->ActiveCamera);
                camera.Rotation.x = std::clamp(camera.Rotation.x + (y - _lastMousePos.y) * 0.15f, -89.0f, 89.0f);
                camera.Rotation.y = std::remainder(camera.Rotation.y - (x - _lastMousePos.x) * 0.15f, 360.0f);
                camera.DirtyFlag = true;
            }
            _lastMousePos = { x, y };
        }

        void OnMouseWheelMove(WPARAM rotation) override
        {
            if (Engine::UI::WantsMouseInput()) return;
            _cameraSpeed = std::clamp(_cameraSpeed * (GET_WHEEL_DELTA_WPARAM(rotation) > 0 ? 1.25f : 0.8f), 0.1f, 10000.0f);
        }

        void OnKeyboardInput(const GameTimer& timer) override
        {
            if (_smoke || bInactive || Engine::UI::WantsKeyboardInput()) return;
            auto* world = Locator.GetModule<SceneManagerModule>()->GetWorld();
            auto& camera = world->GetECS().Get<TransformComponent>(world->ActiveCamera);
            const auto rotation = Matrix::CreateFromYawPitchRoll(
                XMConvertToRadians(camera.Rotation.y), XMConvertToRadians(camera.Rotation.x), 0.0f);
            Vector3 movement = Vector3::Zero;
            if (GetAsyncKeyState('W') & 0x8000) movement -= rotation.Forward();
            if (GetAsyncKeyState('S') & 0x8000) movement += rotation.Forward();
            if (GetAsyncKeyState('A') & 0x8000) movement += rotation.Right();
            if (GetAsyncKeyState('D') & 0x8000) movement -= rotation.Right();
            if (GetAsyncKeyState('Q') & 0x8000) movement -= Vector3::Up;
            if (GetAsyncKeyState('E') & 0x8000) movement += Vector3::Up;
            if (movement.LengthSquared() > 0.0f)
            {
                movement.Normalize();
                const float boost = (GetAsyncKeyState(VK_SHIFT) & 0x8000) ? 4.0f : 1.0f;
                camera.Location += movement * _cameraSpeed * boost * timer.DeltaTime();
                camera.DirtyFlag = true;
            }
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
            _pipeline->ValidateQueued();
            const auto finalMemory=renderer->GetMemoryStatistics();
            Log::Info("MetaRenderGraph","U4 resource allocation after queued frames bytes=",finalMemory.AllocationBytes," count=",finalMemory.AllocationCount," block bytes=",finalMemory.BlockBytes);
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
            Log::Info("MetaRenderGraph", "Smoke passed: 12 presented frames, actual window resize to ", width, "x", height,
                "; D3D12 errors=0; 8 scene serial/parallel pairs + 4 queued frames; animation inputs fixed; host dt=0; compared wind/interaction dt=1/60");
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
        PrepareStartup(smoke);
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
