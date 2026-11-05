#include "../src/BistroProps.h"
#include "App.Base/Physics/JoltPhysicsBackend.h"
#include "App.Base/Modules/SceneManagerModule.h"
#include "App.Base/Systems/DynamicBodyUpdateSystem.h"
#include "Engine.Core/BenchmarkEngine.h"
#include "Engine.Core/IO/MeshImporter.h"
#include "Engine.Core/Jobs/JobSystem.h"
#include "App.Base/App.h"
#include <iostream>
#include <limits>
#include <tuple>

static void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

static void CheckWaveTimeline()
{
    using namespace BistroWaves;
    Settings settings;
    for (uint64_t serial=1; serial<1000; ++serial)
    {
        const auto interval=Interval(settings.Seed,serial);
        Require(interval>=5 && interval<=10 && interval==Interval(settings.Seed,serial), "Invalid/non-reproducible wave interval");
    }
    Require(Interval(settings.Seed,1)!=Interval(settings.Seed+1,1) && Interval(settings.Seed,1)!=Interval(settings.Seed,2), "Interval ignores seed/serial");
    Timeline timeline;
    unsigned launches=0, scatters=0;
    uint32_t scatterSeed=0;
    const auto launch=[&](const Settings&,uint64_t){++launches;};
    const auto scatter=[&](const Settings& s,uint64_t){++scatters; scatterSeed=s.Seed;};
    const auto advance=[&](double dt,const Settings& s){timeline.Advance(dt,true,s,launch,scatter);};
    const double wait=Interval(settings.Seed,1);
    advance(0,settings); Require(launches==0,"Zero time launched a wave");
    advance(wait-0.001,settings); Require(launches==0 && scatters==0,"Wave launched before initial random wait");
    advance(0.0011,settings); Require(launches==1 && scatters==0,"Initial wait did not launch once");
    auto edited=settings; ++edited.Seed;
    advance(ScatterDelay-timeline.Elapsed()-0.001,edited); Require(scatters==0,"Early scatter before one second");
    advance(0.0011,edited);
    Require(scatters==1 && launches==1 && scatterSeed==settings.Seed,"Scatter timing or latched seed failed");
    const auto remaining=timeline.CurrentInterval()-timeline.Elapsed();
    advance(remaining+0.0001,edited);
    Require(timeline.CompletedCycles()==1 && launches==2 && scatters==1 && timeline.LastCompletedCycleSerial()==1,"Cycle completed too early or missed next launch");
    timeline.Advance(0,false,edited,launch,scatter);
    Require(timeline.CompletedCycles()==1 && timeline.GetPhase()==Phase::Waiting,"Disable counted an aborted cycle");
    advance(Interval(edited.Seed,3)+0.0001,edited);
    Require(launches==3 && scatters==1 && timeline.CompletedCycles()==1,"Enable must wait then start a new cycle");
    advance(30,edited);
    Require(scatters==launches-1-(timeline.GetPhase()==Phase::Lift ? 1 : 0) && timeline.CompletedCycles()==launches-2,"Crossed boundaries skipped/duplicated impulses or counted abort");
    const auto a=ScatterVelocity(settings,7,Entity(12));
    Require(a==ScatterVelocity(settings,7,Entity(12)) && a!=ScatterVelocity(settings,8,Entity(12)) &&
        a!=ScatterVelocity(settings,7,Entity(13)) && a!=ScatterVelocity(edited,7,Entity(12)),"Direction seed is not reproducible/identity keyed");
    Require(std::abs(a.Length()-settings.ScatterSpeed)<0.001 && a.Length()>settings.LaunchSpeed,"Scatter is not stronger than launch");
    for (Vector3 current : {Vector3(0,-MaxSpeed,0),Vector3(MaxSpeed,0,0),Vector3(0,MaxSpeed,0)})
        for (uint64_t serial=1; serial<=100; ++serial)
        {
            const auto delta=BoundScatter(ScatterVelocity(settings,serial,Entity(12)),current);
            Require(std::abs(delta.Length()-settings.ScatterSpeed)<0.01 && (current+delta).Length()<=MaxSpeed+0.01,
                "Bounded scatter lost impulse strength or exceeded speed limit");
        }
    for (double bad : {-1.,501.,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()})
    {
        auto invalid=settings; invalid.LaunchSpeed=bad; bool rejected=false;
        try { invalid.Validate(); } catch(const std::invalid_argument&) { rejected=true; }
        Require(rejected,"Invalid wave settings accepted");
    }
    Require(!LaunchSpeed.SetFromString("nan") && !LaunchSpeed.SetFromString("501") &&
        !ScatterSpeed.SetFromString("599") && !ScatterSpeed.SetFromString("901") && !Seed.SetFromString("-1"),"Invalid wave CVar accepted");
    std::cout << "Random 5..10s wait, two single impulses 1s apart, stronger scatter, latched settings, abort/serial and seed passed\n";
}

class WaveTestApp final : public App
{
public:
    WaveTestApp() : App(nullptr) {}
    using App::UpdatePhysics;
    void UseConfiguredSystems(bool enabled) { _AppConfig.StartScene = enabled ? "test" : ""; }
};

static void CheckWaveClock()
{
    WaveTestApp app;
    auto& locator = BenchmarkEngine::GetLocator();
    auto scenes = std::make_shared<SceneManagerModule>();
    auto physics = std::make_shared<PhysicsModule>(app.GetJobs());
    locator.RegisterModule(scenes); locator.RegisterModule(physics); physics->Initialize();
    constexpr float step = 1.f/60;
    for (bool configured : {false, true})
    {
        app.UseConfiguredSystems(configured);
        World world(WorldDesc{}); scenes->OnWorldCreated.Broadcast(world);
        BistroWaves::Timeline timeline;
        unsigned ticks = 0, scatters = 0;
        world.BeforePhysicsStep = [&](float dt)
        {
            ++ticks;
            timeline.Advance(dt, true, BistroWaves::Settings{}, [](const auto&, uint64_t){},
                [&](const auto&, uint64_t){ ++scatters; });
        };
        app.UpdatePhysics(world, step/2); Require(ticks == 0, "Wave ran on fractional physics time");
        app.UpdatePhysics(world, step/2); Require(ticks == 1, "Wave missed fixed tick");
        world.SetPaused(true); app.UpdatePhysics(world, 100);
        Require(ticks == 1, "Pause advanced the wave clock"); world.SetPaused(false);
        world.SetTimeScale(0); app.UpdatePhysics(world, 100); Require(ticks == 1, "TimeScale zero advanced wave");
        world.SetTimeScale(2); app.UpdatePhysics(world, step/2); Require(ticks == 2, "Wave ignored TimeScale");
        world.SetTimeScale(1); app.UpdatePhysics(world, step*10); Require(ticks == 10, "Catch-up work was not bounded");
        app.UpdatePhysics(world, step*1.01f); Require(ticks == 13, "Catch-up discarded fixed wave time");
        app.UpdatePhysics(world, step*1800);
        while (ticks < 1813) app.UpdatePhysics(world, step*0.001f);
        Require(timeline.CompletedCycles() >= 2 && scatters == timeline.StartedCycles()-(timeline.GetPhase()==BistroWaves::Phase::Lift ? 1 : 0), "Catch-up lost/repeated a scatter or cycle");
        scenes->OnWorldDestroyed.Broadcast(world);
    }
    physics->Uninitialize(); locator.UnregisterModule<PhysicsModule>(); locator.UnregisterModule<SceneManagerModule>();
    std::cout << "Wave default/configured fixed pipeline, pause, TimeScale and retained catch-up passed\n";
}

static void CheckParticipants(PhysicsModule& physics, SceneManagerModule& scenes)
{
    World world(WorldDesc{}); scenes.OnWorldCreated.Broadcast(world);
    physics.ConfigureWorld(world, BistroPhysics::Settings());
    auto& ecs = world.GetECS();
    const auto create = [&](Vector3 position, Vector3 extent, PhysicsMotionType motion)
    {
        auto e = ecs.CreateEntity(); e.AddComponent<TransformComponent>(position);
        e.AddComponent<RigidBodyComponent>().MotionType = motion;
        e.AddComponent<ColliderComponent>(BoxShapeDesc{extent}); return e.GetId();
    };
    const auto floor = create({0,-50,0}, {1000,50,1000}, PhysicsMotionType::Static);
    const auto prop = create({-100,10,0}, {10,10,10}, PhysicsMotionType::Dynamic);
    const auto heavy = create({100,20,0}, {20,20,20}, PhysicsMotionType::Dynamic);
    const auto character = create({500,10,0}, {10,10,10}, PhysicsMotionType::Dynamic);
    for (int i=0;i<300;++i) physics.Step(world,1.f/60);
    const auto handle = [&](Entity e) { return ecs.Get<RigidBodyComponent>(e).Body; };
    Vector3 velocity; bool active = true;
    Require(physics.TryGetBodyVelocity(world,handle(prop),velocity,active) && !active, "Fixture prop did not sleep");
    BistroWaves::Controller waves;
    waves.Register(prop); waves.Register(prop); waves.Register(heavy); waves.Register(floor);
    Require(waves.ParticipantCount() == 3, "Duplicate participant registered twice");
    while (waves.GetTimeline().StartedCycles()==0) { waves.Tick(world,physics,1.f/60); if (waves.GetTimeline().StartedCycles()==0) physics.Step(world,1.f/60); }
    Require(physics.TryGetBodyVelocity(world,handle(prop),velocity,active) && active, "Wave failed to wake sleeping prop");
    Require(physics.TryGetBodyVelocity(world,handle(character),velocity,active) && !active, "Unregistered character was woken");
    physics.Step(world,1.f/60);
    Vector3 heavyVelocity;
    Require(physics.TryGetBodyVelocity(world,handle(prop),velocity,active) && velocity.y > 450, "Wave did not lift sleeping prop");
    Require(physics.TryGetBodyVelocity(world,handle(heavy),heavyVelocity,active) && std::abs(heavyVelocity.y-velocity.y)<0.05,
        "Launch impulse depends on body mass");
    Require(physics.TryGetBodyVelocity(world,handle(character),velocity,active) && velocity.Length()<0.1f,
        "Unregistered dynamic character received a wave");
    Require(!physics.AddBodyVelocityImpulse(world,handle(floor),{100,0,0},600), "Static body accepted drive");
    for (auto e : {prop,heavy}) Require(physics.AddBodyVelocityImpulse(world,handle(e),{100,0,0},600), "Impulse rejected");
    physics.TryGetBodyVelocity(world,handle(prop),velocity,active);
    physics.TryGetBodyVelocity(world,handle(heavy),heavyVelocity,active);
    Require(std::abs(velocity.x-100)<0.01 && std::abs(heavyVelocity.x-100)<0.01, "Impulse delta speed depends on mass or cm/m");
    physics.AddBodyVelocityImpulse(world,handle(prop),{100000,0,0},600);
    physics.TryGetBodyVelocity(world,handle(prop),velocity,active);
    Require(velocity.Length() <= 600.01f, "Drive exceeded speed cap");
    bool rejected = false;
    try { physics.AddBodyVelocityImpulse(world,handle(prop),{NAN,0,0},600); } catch(const std::invalid_argument&) { rejected=true; }
    Require(rejected, "Nonfinite impulse accepted");
    waves.Unregister(prop); Require(waves.ParticipantCount()==2, "Participant unregister failed");
    DynamicBodyUpdateSystem sync(physics);
    for(float pitch : {-90.f,-89.9999f,89.9999f,90.f}) for(float yaw : {-179.f,72.f})
    {
        auto e=ecs.CreateEntity(); e.AddComponent<TransformComponent>();
        auto& body=e.AddComponent<RigidBodyComponent>(); body.MotionType=PhysicsMotionType::Dynamic;
        PhysicsBodyDesc desc; desc.MotionType=PhysicsMotionType::Dynamic;
        desc.Rotation=Quaternion::CreateFromYawPitchRoll(XMConvertToRadians(yaw),XMConvertToRadians(pitch),XMConvertToRadians(-28.f));
        body.Body=physics.CreateBody(world,desc);
        sync.Tick(world,0);
        const auto& angles=ecs.Get<TransformComponent>(e.GetId()).Rotation;
        const auto rendered=Quaternion::CreateFromYawPitchRoll(XMConvertToRadians(angles.y),XMConvertToRadians(angles.x),XMConvertToRadians(angles.z));
        for(const auto axis : {Vector3::Right,Vector3::Up,Vector3::Forward})
            Require(Vector3::Distance(Vector3::Transform(axis,rendered),Vector3::Transform(axis,desc.Rotation))<0.00001f,
                "Physics/render quaternion mismatch near Euler singularity");
    }
    scenes.OnWorldDestroyed.Broadcast(world);
    std::cout << "Explicit participants, sleeping wake-up, static/character exclusion, mass, cm/m and speed cap passed\n";
}

static void CheckBudgets()
{
    const std::pair<double, size_t> cases[] = {{0,346},{0.5,346},{1,346},{1.25,433},{2,692},{16,5536}};
    for (const auto& [scale, expected] : cases)
        Require(BistroPhysics::GetCount(346, scale) == expected, "Wrong spawn count");
    for (double bad : {-1., 17., std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
    {
        bool rejected = false;
        try { BistroPhysics::GetCount(346, bad); } catch (const std::exception&) { rejected = true; }
        Require(rejected, "Invalid scale accepted");
    }
    bool rejected = false;
    try { BistroPhysics::GetCount(SIZE_MAX, 16); } catch (const std::exception&) { rejected = true; }
    Require(rejected, "Count overflow accepted");
    Require(!BistroPhysics::CountScale.SetFromString("nan") && !BistroPhysics::CountScale.SetFromString("17"), "Invalid CVar accepted");
}

static void CheckBakedPivot(JobSystem& jobs)
{
    JoltPhysicsBackend backend;
    auto invalid = BistroPhysics::Settings();
    invalid.MaxContactConstraints = UINT32_MAX;
    bool rejected = false;
    try { backend.Initialize(jobs, invalid); } catch (const std::invalid_argument&) { rejected = true; }
    Require(rejected, "Temporary allocator size overflow accepted");
    backend.Initialize(jobs, BistroPhysics::Settings());
    auto ground = std::make_shared<PhysicsTriangleMesh>();
    ground->Vertices = {{0,0,-500},{0,0,500},{2000,0,500},{2000,0,-500}};
    ground->Triangles = {{0,1,2},{0,2,3}};
    PhysicsBodyDesc floor;
    floor.Shape = TriangleMeshShapeDesc{ground};
    Require(backend.CreateBody(floor).IsValid(), "Triangle floor failed");
    auto geometry = std::make_shared<PhysicsConvexCompound>();
    geometry->Hulls.emplace_back();
    const Vector3 center(1000,200,0);
    for (float x : {-10.f,10.f}) for (float y : {-20.f,20.f}) for (float z : {-10.f,10.f})
        geometry->Hulls[0].push_back(center + Vector3(x,y,z));
    PhysicsBodyDesc desc;
    desc.Shape = ConvexCompoundShapeDesc{geometry};
    desc.MotionType = PhysicsMotionType::Dynamic;
    desc.Rotation = DirectX::SimpleMath::Quaternion::CreateFromYawPitchRoll(0,0,0.3f);
    desc.Position = center - Vector3::Transform(center, desc.Rotation);
    const auto body = backend.CreateBody(desc);
    Require(body.IsValid(), "Baked hull failed");
    Require(backend.OverlapsBox(center, {11,21,11}), "Placement query missed existing hull");
    Require(!backend.OverlapsBox(center + Vector3(100,0,0), {11,21,11}), "Placement query rejected free space");
    PhysicsRayHit hit;
    Require(backend.RayCastStatic(center,{0,-500,0},hit) && std::abs(hit.Position.y)<0.01 && hit.Normal.y>0.9f &&
        !backend.RayCastStatic({3000,200,0},{0,-500,0},hit),
        "Static ray must select the real floor, not empty bounds or dynamic props");
    backend.Step(1.f/60);
    Vector3 p; DirectX::SimpleMath::Quaternion q;
    Require(backend.TryGetBodyTransform(body,p,q), "Missing pivot body");
    const auto first = Vector3::Transform(center,q) + p;
    Require(first.y < 200 && first.y > 199, "Physics must use meters and terrestrial gravity");
    for (int i=0;i<300;++i) backend.Step(1.f/60);
    Require(backend.TryGetBodyTransform(body,p,q), "Missing settled body");
    const auto settled = Vector3::Transform(center,q) + p;
    Require(std::abs(settled.x-1000)<100 && settled.y>8 && settled.y<25 && std::abs(settled.z)<100,
        "Baked mesh rotated about scene origin or passed through floor");
    std::cout << "Baked pivot, cm/m gravity, triangle contacts and placement queries passed\n";
}

int main(int argc, char** argv)
{
    try
    {
        std::cout << std::unitbuf;
        CheckBudgets();
        CheckWaveTimeline();
        CheckWaveClock();
        const bool unitOnly = argc > 1 && std::string_view(argv[1]) == "--unit-only";
        const double scale = argc > 1 && !unitOnly ? std::stod(argv[1]) : 1;
        const unsigned cycles = argc > 3 ? static_cast<unsigned>(std::stoul(argv[3])) : 3;
        Require(cycles >= 1 && cycles <= 30, "Test cycles must be 1..30");
        Require(BistroPhysics::CountScale.SetValue(scale), "Invalid test scale");
        JobSystem jobs(argc > 2 ? static_cast<unsigned>(std::stoul(argv[2])) : 4);
        CheckBakedPivot(jobs);
        BenchmarkEngine engine;
        auto scenes = std::make_shared<SceneManagerModule>();
        BenchmarkEngine::GetLocator().RegisterModule(scenes);
        PhysicsModule physics(jobs);
        physics.Initialize();
        CheckParticipants(physics, *scenes);
        if (unitOnly) { physics.Uninitialize(); return 0; }
        World world(WorldDesc{});
        scenes->OnWorldCreated.Broadcast(world);
        physics.ConfigureWorld(world, BistroPhysics::Settings());
        size_t base = 0, expected = 0;
        std::vector<std::tuple<Entity,Vector3,Vector3>> centers;
        BistroWaves::Controller waves;
        for (const auto* part : {"Exterior/exterior.obj", "Interior/interior.obj"})
        {
            Engine::Core::MeshImportOptions options;
            options.bCache = true;
            const auto mesh = Engine::Core::MeshImporter::ImportSingleMeshAsset(std::filesystem::path(ASSETS_FOLDER) / "Scenes/Amazon Lumberyard Bistro" / part, options);
            Require(bool(mesh), "Missing real Bistro geometry");
            const auto partition = PartitionBistroProps(*mesh);
            base += partition.Props.size();
            expected += BistroPhysics::GetCount(partition.Props.size(), scale);
            std::cout << part << ": " << partition.Props.size() << " originals; static sections=" << partition.StaticSubMeshes.size() << std::endl;
            const auto before = world.GetECS().GetPool<RigidBodyComponent>().Size();
            CreateBistroPropEntities(world, part, StaticMeshRenderComponent({},{}), *mesh, partition, &physics, &waves);
            auto& ecs = world.GetECS();
            ecs.ForEach<NameComponent,StaticMeshRenderComponent,RigidBodyComponent>([&](Entity entity, const NameComponent& name, const StaticMeshRenderComponent& render, const RigidBodyComponent& body)
            {
                if (body.MotionType != PhysicsMotionType::Dynamic) return;
                if (std::find_if(centers.begin(),centers.end(),[&](const auto& v){return std::get<0>(v)==entity;}) != centers.end()) return;
                const Vector3 center(BistroPhysics::Bounds(*mesh,render.SubMeshIndices).Center);
                centers.emplace_back(entity,center,center+ecs.Get<TransformComponent>(entity).Location);
            });
            Require(ecs.GetPool<RigidBodyComponent>().Size() == before + BistroPhysics::GetCount(partition.Props.size(),scale) + 1, "Lost bodies");
            std::vector<unsigned> visits(mesh->GetSubMeshCount());
            ecs.ForEach<NameComponent,StaticMeshRenderComponent>([&](Entity, const NameComponent& name, const StaticMeshRenderComponent& render)
            {
                if (name.value == part || std::any_of(partition.Props.begin(),partition.Props.end(),[&](const auto& prop){return prop.Name==name.value;}))
                    for (auto index : render.SubMeshIndices) ++visits.at(index);
            });
            for (auto visits : visits) Require(visits == 1, "Source rendered twice or missing");
        }
        Require(base == 379, "Bistro source object grouping changed");
        auto& ecs=world.GetECS();
        ecs.ForEach<RigidBodyComponent>([](Entity,const RigidBodyComponent& body){Require(body.Body.IsValid(),"Silently missing Jolt body");});
        DynamicBodyUpdateSystem sync(physics);
        Require(waves.ParticipantCount() == expected, "Only spawned props must opt into waves");
        float peakSpeed = 0;
        unsigned step=0;
        const size_t maxTicks=600*(cycles+1);
        std::vector<Vector3> previousVelocities(expected);
        float minimumScatter=FLT_MAX;
        float minimumLaunch=FLT_MAX;
        size_t chordCrossings=0;
        std::vector<Vector3> previousCenters;
        std::vector<bool> leftGeometry(centers.size(),false);
        size_t outside=0;
        for(const auto& item:centers) previousCenters.push_back(std::get<2>(item));
        while (waves.GetTimeline().CompletedCycles()<cycles)
        {
            Require(step++ < maxTicks,"Wave timeline exceeded bounded 5..10s intervals");
            const auto beforeSerial=waves.GetTimeline().StartedCycles();
            const auto beforePhase=waves.GetTimeline().GetPhase();
            waves.Tick(world,physics,1.f/60);
            const bool launched=waves.GetTimeline().StartedCycles()!=beforeSerial;
            const bool scattered=!launched && beforePhase==BistroWaves::Phase::Lift && waves.GetTimeline().GetPhase()==BistroWaves::Phase::Fall;
            if (launched || scattered)
            {
                size_t index=0;
                ecs.ForEach<RigidBodyComponent>([&](Entity entity,const RigidBodyComponent& body)
                {
                    if(body.MotionType!=PhysicsMotionType::Dynamic) return;
                    Vector3 v; bool active; physics.TryGetBodyVelocity(world,body.Body,v,active);
                    const auto delta=(v-previousVelocities[index++]).Length();
                    const auto wanted=scattered ? BistroWaves::ScatterSpeed.GetValue() : BistroWaves::LaunchSpeed.GetValue();
                    if (scattered) minimumScatter=(std::min)(minimumScatter,delta);
                    else minimumLaunch=(std::min)(minimumLaunch,delta);
                    const bool invalid=scattered ? std::abs(delta-wanted)>=0.1 : delta>wanted+0.1;
                    if (invalid)
                    {
                        std::cout << "Clipped " << ecs.Get<NameComponent>(entity).value << " cycle=" << beforeSerial << " before speed=" << previousVelocities[index-1].Length()
                            << " after speed=" << v.Length() << " delta=" << delta << '\n';
                        const auto tracked=std::find_if(centers.begin(),centers.end(),[&](const auto& c){return std::get<0>(c)==entity;});
                        Vector3 p; Quaternion q; physics.TryGetBodyTransform(world,body.Body,p,q);
                        const auto pos=Vector3::Transform(std::get<1>(*tracked),q)+p;
                        const auto start=std::get<2>(*tracked);
                        std::cout << "  start=" << start.x << ',' << start.y << ',' << start.z << " now=" << pos.x << ',' << pos.y << ',' << pos.z << '\n';
                    }
                    Require(!invalid,"Scatter clipped or launch exceeded the requested impulse");
                });
            }
            physics.Step(world,1.f/60); sync.Schedule(jobs,world,1.f/60).Complete();
            size_t velocityIndex=0;
            ecs.ForEach<RigidBodyComponent>([&](Entity,const RigidBodyComponent& body)
            {
                Vector3 v; bool active;
                Require(physics.TryGetBodyVelocity(world,body.Body,v,active) && std::isfinite(v.LengthSquared()), "Nonfinite velocity");
                if(body.MotionType==PhysicsMotionType::Dynamic) previousVelocities[velocityIndex++]=v;
                peakSpeed = (std::max)(peakSpeed,v.Length());
                Require(v.Length() <= BistroWaves::MaxSpeed + 0.1f, "Runaway wave velocity");
            });
            Require(ecs.GetPool<RigidBodyComponent>().Size() == expected+2, "Wave changed body count");
            size_t centerIndex=0;
            for (const auto& [entity,center,start] : centers)
            {
                const auto& t=ecs.Get<TransformComponent>(entity);
                Require(std::isfinite(t.Location.LengthSquared()) && std::isfinite(t.Rotation.LengthSquared()), "Nonfinite pose");
                Vector3 p; Quaternion rotation;
                Require(physics.TryGetBodyTransform(world,ecs.Get<RigidBodyComponent>(entity).Body,p,rotation),"Missing physical pose");
                const auto c=Vector3::Transform(center,rotation)+p;
                const auto rendered=Vector3::Transform(center,Quaternion::CreateFromYawPitchRoll(XMConvertToRadians(t.Rotation.y),XMConvertToRadians(t.Rotation.x),XMConvertToRadians(t.Rotation.z)))+t.Location;
                if(Vector3::Distance(c,rendered)>=0.5f) std::cout << "Pivot mismatch " << ecs.Get<NameComponent>(entity).value << " cm=" << Vector3::Distance(c,rendered) << '\n';
                Require(Vector3::Distance(c,rendered)<0.5f,"Physics/render pivot or rotation mismatch");
                const auto& name=ecs.Get<NameComponent>(entity).value;
                const auto delta=c-previousCenters[centerIndex];
                PhysicsRayHit hit;
                if ((name.find("Bottle")!=std::string::npos || name.find("bottle")!=std::string::npos) && delta.LengthSquared()>0.000001f &&
                    physics.RayCastStatic(world,previousCenters[centerIndex],delta,hit) && (c-hit.Position).Dot(hit.Normal)<-0.5f)
                {
                    ++chordCrossings;
                    if(chordCrossings<=3) std::cout << "Static chord intersection candidate: " << name << " t=" << step/60. << " hit=" << hit.Position.x << ',' << hit.Position.y << ',' << hit.Position.z << '\n';
                }
                if(c.y<=-1000 && !leftGeometry[centerIndex])
                {
                    leftGeometry[centerIndex]=true; ++outside;
                    if(outside<=3) std::cout << "Left open geometry: " << name << " t=" << step/60. << " at=" << c.x << ',' << c.y << ',' << c.z << '\n';
                }
                previousCenters[centerIndex++]=c;
            }
        }
        Require(waves.GetTimeline().CompletedCycles() == cycles, "Real Bistro did not complete the requested full waves");
        size_t moved=0;
        for (const auto& [entity,center,start] : centers)
        {
            const auto& t=ecs.Get<TransformComponent>(entity);
            const auto q=DirectX::SimpleMath::Quaternion::CreateFromYawPitchRoll(DirectX::XMConvertToRadians(t.Rotation.y),DirectX::XMConvertToRadians(t.Rotation.x),DirectX::XMConvertToRadians(t.Rotation.z));
            const auto current=Vector3::Transform(center,q)+t.Location;
            Require(std::isfinite(current.x)&&std::isfinite(current.y)&&std::isfinite(current.z),"Nonfinite physics transform");
            if (Vector3::Distance(current,start)>1) ++moved;
        }
        scenes->OnWorldDestroyed.Broadcast(world);
        physics.Uninitialize();
        std::cout << "Bistro: " << base << " original objects, " << expected << " dynamic bodies, 2 static meshes, scale=" << scale << ", moved=" << moved << "; " << step << " ticks, " << cycles << " full waves, peak speed=" << peakSpeed << " cm/s, minimum applied scatter=" << minimumScatter << " cm/s, minimum launch=" << minimumLaunch << ", outside modeled geometry=" << outside << ", static chord candidates=" << chordCrossings << " passed\n";
        return 0;
    }
    catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
