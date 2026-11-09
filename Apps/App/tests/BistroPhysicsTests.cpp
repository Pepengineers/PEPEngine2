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
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <Jolt/Jolt.h>
#include <Jolt/Core/IssueReporting.h>

#ifdef JPH_ENABLE_ASSERTS
static std::atomic<unsigned> JoltAssertions{0};
static bool RecordJoltAssert(const char* expression, const char*, const char* file, JPH::uint line)
{
    if (JoltAssertions.fetch_add(1) < 4)
        std::cerr << "Jolt assertion: " << file << ':' << line << ": " << expression << '\n';
    return false;
}
#endif

static void RecordJoltTrace(const char* format, ...)
{
    va_list args; va_start(args,format); std::vfprintf(stderr,format,args); va_end(args);
    std::fputc('\n',stderr); std::fflush(stderr);
}

static void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

template<class RayCast>
static bool BelowIndoorFloor(const Vector3& center, RayCast cast, PhysicsRayHit& support)
{
    if (center.y>=50 || !cast({center.x,65,center.z},support) || support.Normal.y<=.5f ||
        (std::abs(support.Position.y-39.622f)>=.2f && std::abs(support.Position.y-51.803f)>=.2f) ||
        center.y>=support.Position.y-2) return false;
    const auto settings=BistroPhysics::Settings();
    const float margin=2*settings.PenetrationSlopMeters*settings.WorldUnitsPerMeter;
    // A body beside the raised floor may overlap its edge within the allowed contact slop.
    for (const Vector3 offset : {Vector3(margin,0,0),Vector3(-margin,0,0),Vector3(0,0,margin),Vector3(0,0,-margin)})
    {
        PhysicsRayHit neighbor;
        if (!cast(Vector3(center.x,65,center.z)+offset,neighbor) || neighbor.Normal.y<=.5f ||
            std::abs(neighbor.Position.y-support.Position.y)>.2f) return false;
    }
    return true;
}

static void CheckFloorProbe()
{
    const auto step=[](const Vector3& origin, PhysicsRayHit& hit)
    {
        hit.Position={origin.x,origin.x>0 ? 51.803f : 39.622f,origin.z};
        hit.Normal=Vector3::UnitY;
        return true;
    };
    PhysicsRayHit hit;
    Require(!BelowIndoorFloor({.0512f,43.9014f,0},step,hit),"Floor probe misclassified a body touching the side of a step");
    Require(BelowIndoorFloor({10,43,0},step,hit) && BelowIndoorFloor({-10,20,0},step,hit),
        "Floor probe missed penetration inside either floor level");
    Require(!BelowIndoorFloor({-10,44,0},step,hit),"Floor probe rejected a body above its support");
}

static void CheckWaveTimeline()
{
    using namespace BistroWaves;
    Settings settings;
    Require(settings.ScatterSpeed==2100 && ScatterSpeed.GetValue()==settings.ScatterSpeed,"Default scatter speed must be tripled to 2100 cm/s");
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
    advance(0.3-timeline.Elapsed()-0.001,edited); Require(scatters==0,"Early scatter before 0.3 seconds");
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
        !ScatterSpeed.SetFromString("1799") && !ScatterSpeed.SetFromString("2701") && !Seed.SetFromString("-1"),"Invalid wave CVar accepted");
    std::cout << "Random 5..10s wait, two single impulses 0.3s apart, stronger scatter, latched settings, abort/serial and seed passed\n";
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
    const auto footprint=BistroPhysics::SpawnFootprint({{0,0,0},{100,0,100},{0,0,100},{100,0,0},{50,0,50},{0,0,0}});
    Require(BistroPhysics::FitsFootprint(footprint,{50,0,50},{49,1,49}) &&
        !BistroPhysics::FitsFootprint(footprint,{99,0,50},{2,1,2}) &&
        !BistroPhysics::FitsFootprint(footprint,{200,0,50},{1,1,1}), "Indoor footprint admits overhanging copies");
    bool degenerate=false;
    try { BistroPhysics::SpawnFootprint({{0,0,0},{1,0,0},{2,0,0}}); } catch(const std::invalid_argument&) { degenerate=true; }
    Require(degenerate,"Collinear spawn footprint accepted");
    const auto diagonal=BistroPhysics::SpawnFootprint({{-100,0,0},{0,0,100},{100,0,0},{0,0,-100}});
    Require(BistroPhysics::FitsFootprint(diagonal,{0,0,0},{40,1,40}) &&
        !BistroPhysics::FitsFootprint(diagonal,{50,0,20},{20,1,20}),
        "Angled indoor boundary admits a box whose center is inside but corner is outside");
    const std::pair<double, size_t> cases[] = {{0,947},{0.5,947},{1,947},{1.25,1174},{2,1852},{16,14522}};
    for (const auto& [scale, expected] : cases)
        Require(BistroPhysics::GetCount(905, scale, 42) == expected, "Wrong spawn count");
    for (double bad : {-1., 17., std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
    {
        bool rejected = false;
        try { BistroPhysics::GetCount(905, bad, 42); } catch (const std::exception&) { rejected = true; }
        Require(rejected, "Invalid scale accepted");
    }
    Require(BistroPhysics::GetCount(0,16,42)==42 && BistroPhysics::GetCount(905,16,0)==14480,
        "Tableware multiplier depends on furniture count");
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

static void CheckBoxProxies(JobSystem& jobs)
{
    using namespace Engine::Core;
    const auto boxMesh = [](Vector3 center, Vector3 half, Quaternion rotation)
    {
        SubMesh part;
        for (const auto& corner : {Vector3(-1,-1,-1), Vector3(1,-1,-1), Vector3(1,1,-1), Vector3(-1,1,-1),
            Vector3(-1,-1,1), Vector3(1,-1,1), Vector3(1,1,1), Vector3(-1,1,1)})
        {
            Vertex vertex;
            vertex.Position = Vector3::Transform(corner * half, rotation) + center;
            part.Vertices.push_back(vertex);
        }
        part.Indices = {0,2,1,0,3,2,4,5,6,4,6,7,0,4,7,0,7,3,1,2,6,1,6,5,0,1,5,0,5,4,3,7,6,3,6,2};
        DirectX::BoundingBox::CreateFromPoints(part.Bounds, part.Vertices.size(), &part.Vertices.front().Position, sizeof(Vertex));
        return part;
    };
    for (const auto& rotation : {Quaternion::Identity, Quaternion::CreateFromYawPitchRoll(0.7f, 0, 0),
        Quaternion::CreateFromYawPitchRoll(0.7f, 0.8f, -0.3f)})
    {
        const Vector3 origin(1000, 200, -400);
        const Mesh bottle({boxMesh(origin, {4,12,3}, rotation),
            boxMesh(origin + Vector3::Transform({0,16,0}, rotation), {1,4,1}, rotation)}, {});
        const auto shape = BistroPhysics::PropShape(bottle, {"Paris_LiquorBottle_Test", {0,1}});
        const auto* compound = std::get_if<CompoundShapeDesc>(&shape);
        Require(compound && compound->Geometry->Children.size() == 1, "Bottle proxy must contain exactly one box");
        const auto& child = compound->Geometry->Children.front();
        const auto* box = std::get_if<BoxShapeDesc>(&child.Shape);
        Require(box != nullptr, "Bottle proxy retained non-box geometry");
        Quaternion inverse; child.Rotation.Conjugate(inverse);
        for (const auto& part : bottle.GetSubMeshes()) for (const auto& vertex : part.Vertices)
        {
            const auto local = Vector3::Transform(vertex.Position - child.Position, inverse);
            Require(std::abs(local.x) <= box->HalfExtent.x + 0.01f &&
                std::abs(local.y) <= box->HalfExtent.y + 0.01f && std::abs(local.z) <= box->HalfExtent.z + 0.01f,
                "Translated or tilted bottle proxy excludes a source vertex");
        }
        JoltPhysicsBackend backend;
        backend.Initialize(jobs, BistroPhysics::Settings());
        PhysicsBodyDesc body; body.Shape = shape;
        Require(backend.CreateBody(body).IsValid() && backend.OverlapsBox(origin, {1,1,1}),
            "Bottle proxy lost its baked position when cooked");
    }
    for (const auto& points : {std::vector<Vector3>{}, std::vector<Vector3>{{NAN,0,0}},
        std::vector<Vector3>{{0,0,0},{0,std::numeric_limits<float>::infinity(),0}}})
    {
        bool rejected = false;
        try { BistroPhysics::BoxProxy(points); } catch (const std::invalid_argument&) { rejected = true; }
        Require(rejected, "Empty or nonfinite box proxy accepted");
    }
    const auto thin = BistroPhysics::BoxProxy({{-2,0,-3},{2,0,-3},{2,0,3},{-2,0,3}});
    const auto half = std::get<BoxShapeDesc>(thin.Shape).HalfExtent;
    Require(half.x >= 0.5f && half.y >= 0.5f && half.z >= 0.5f &&
        std::abs((std::min)({half.x,half.y,half.z}) - 0.5f) < 0.001f,
        "Thin box proxy did not retain a 0.5 cm minimum half-extent");

    const Vector3 origin(1000, 0, -400);
    const auto rotation = Quaternion::CreateFromYawPitchRoll(0.4f, 0, 0);
    std::vector<SubMesh> parts;
    const auto add = [&](Vector3 center, Vector3 half)
    { parts.push_back(boxMesh(Vector3::Transform(center, rotation) + origin, half, rotation)); };
    add({0,80,0}, {50,3,40});
    for (float x : {-40.f,40.f}) for (float z : {-30.f,30.f}) add({x,38.5f,z}, {3,38.5f,3});
    PhysicsBodyDesc table;
    table.Shape = BistroPhysics::PropShape(Mesh(std::move(parts), {}), {"Paris_Table_Test", {0,1,2,3,4}});
    const auto& children = std::get<CompoundShapeDesc>(table.Shape).Geometry->Children;
    Require(children.size() > 1 && children.size() <= 12, "Table proxy lost its bounded compound structure");
    for (const auto& child : children)
        Require(!std::holds_alternative<ConvexCompoundShapeDesc>(child.Shape), "Table proxy retained a convex hull");
    JoltPhysicsBackend backend;
    backend.Initialize(jobs, BistroPhysics::Settings());
    Require(backend.CreateBody(table).IsValid(), "Table box proxies could not be cooked");
    Require(!backend.OverlapsBox(origin + Vector3(0,30,0), {5,5,5}), "Table box proxies filled the space underneath");
    for (float x : {-40.f,40.f}) for (float z : {-30.f,30.f})
        Require(backend.OverlapsBox(origin + Vector3::Transform({x,30,z}, rotation), {1,1,1}),
            "Table box proxies lost a supporting leg");
    PhysicsRayHit hit;
    Require(backend.RayCastStatic(origin + Vector3(0,120,0), {0,-150,0}, hit) && std::abs(hit.Position.y - 83) < 2,
        "Table box proxies lost or moved the supporting tabletop");
    std::cout << "Bottle box containment, baked pivots, invalid input, minimum thickness and table gaps/support passed\n";
}

int main(int argc, char** argv)
{
    try
    {
        std::cout << std::unitbuf;
        CheckBudgets();
        CheckFloorProbe();
        CheckWaveTimeline();
        CheckWaveClock();
        const bool unitOnly = argc > 1 && std::string_view(argv[1]) == "--unit-only";
        const double scale = argc > 1 && !unitOnly ? std::stod(argv[1]) : 1;
        const unsigned cycles = argc > 3 ? static_cast<unsigned>(std::stoul(argv[3])) : 3;
        Require(cycles >= 1 && cycles <= 30, "Test cycles must be 1..30");
        Require(BistroPhysics::CountScale.SetValue(scale), "Invalid test scale");
        JobSystem jobs(argc > 2 ? static_cast<unsigned>(std::stoul(argv[2])) : 4);
        CheckBakedPivot(jobs);
        CheckBoxProxies(jobs);
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
#ifdef JPH_ENABLE_ASSERTS
        JPH::AssertFailed = RecordJoltAssert;
#endif
        JPH::Trace = RecordJoltTrace;
        size_t base = 0, expected = 0;
        std::vector<std::tuple<Entity,Vector3,Vector3>> centers;
        BistroWaves::Controller waves;
        for (const auto* part : {"Exterior/exterior.obj", "Interior/interior.obj"})
        {
            Engine::Core::MeshImportOptions options;
            options.bCache = true;
            const auto mesh = Engine::Core::MeshImporter::ImportSingleMeshAsset(std::filesystem::path(ASSETS_FOLDER) / "Scenes/Amazon Lumberyard Bistro" / part, options);
            Require(bool(mesh), "Missing real Bistro geometry");
            const auto partition = PartitionBistroProps(*mesh, std::string_view(part) == "Interior/interior.obj");
            base += partition.Props.size();
            expected += BistroPhysics::GetCount(partition, scale);
            std::cout << part << ": " << partition.Props.size() << " originals; static sections=" << partition.StaticSubMeshes.size() << std::endl;
            const auto before = world.GetECS().GetPool<RigidBodyComponent>().Size();
            CreateBistroPropEntities(world, part, StaticMeshRenderComponent({},{}), *mesh, partition, jobs, &physics, &waves);
            auto& ecs = world.GetECS();
            Require(ecs.GetPool<RigidBodyComponent>().Size() == before + BistroPhysics::GetCount(partition,scale) + 1,
                "Expected one static body for each of the walkway and room");
            ecs.ForEach<NameComponent,StaticMeshRenderComponent,RigidBodyComponent>([&](Entity entity, const NameComponent& name, const StaticMeshRenderComponent& render, const RigidBodyComponent& body)
            {
                if (body.MotionType != PhysicsMotionType::Dynamic) return;
                if (std::find_if(centers.begin(),centers.end(),[&](const auto& v){return std::get<0>(v)==entity;}) != centers.end()) return;
                const auto sourceBounds = BistroPhysics::Bounds(*mesh,render.SubMeshIndices);
                if (name.value.find("_PhysicsCopy_") == std::string::npos &&
                    (name.value.find("Table") != std::string::npos || name.value.find("Chair") != std::string::npos))
                    for (const auto& child : std::get<CompoundShapeDesc>(ecs.Get<ColliderComponent>(entity).Shape).Geometry->Children)
                        if (const auto* box = std::get_if<BoxShapeDesc>(&child.Shape))
                            for (float x : {-1.f,1.f}) for (float y : {-1.f,1.f}) for (float z : {-1.f,1.f})
                            {
                                const auto point = Vector3::Transform(box->HalfExtent*Vector3(x,y,z),child.Rotation)+child.Position;
                                Require(point.y >= sourceBounds.Center.y-sourceBounds.Extents.y-2 &&
                                    point.y <= sourceBounds.Center.y+sourceBounds.Extents.y+2,
                                    "Furniture proxy exceeds authored vertical envelope");
                            }
                const Vector3 center(sourceBounds.Center);
                const auto& placement = ecs.Get<TransformComponent>(entity);
                const auto placedRotation = Quaternion::CreateFromYawPitchRoll(XMConvertToRadians(placement.Rotation.y), XMConvertToRadians(placement.Rotation.x), XMConvertToRadians(placement.Rotation.z));
                const auto placedCenter = Vector3::Transform(center, placedRotation)+placement.Location;
                Require(placedCenter.y < 421 && placedCenter.y > 39, "Prop spawned outside the restaurant floor/ceiling");
                centers.emplace_back(entity,center,placedCenter);
            });
            std::vector<unsigned> visits(mesh->GetSubMeshCount());
            ecs.ForEach<NameComponent,StaticMeshRenderComponent>([&](Entity, const NameComponent& name, const StaticMeshRenderComponent& render)
            {
                if (name.value == part || std::any_of(partition.Props.begin(),partition.Props.end(),[&](const auto& prop){return prop.Name==name.value;}))
                    for (auto index : render.SubMeshIndices) ++visits.at(index);
            });
            for (auto visits : visits) Require(visits == 1, "Source rendered twice or missing");
        }
        Require(base == 947, "Bistro source object grouping changed");
        PhysicsRayHit street;
        Require(!physics.RayCastStatic(world,{800,30,-650},{0,-100,0},street),
            "Street away from the walkway still has a Jolt ground collider");
        for (const auto& edge : BistroRoutes::People().Edges)
            for (const auto& point : edge.Points)
                Require(physics.RayCastStatic(world,point+Vector3(0,5,0),{0,-55,0},street) && street.Normal.y > 0 &&
                    std::abs(street.Position.y+BistroRoutes::PeopleGroundOffset-point.y)<5,
                    "Outdoor walkway lost support or deviated from the baked route height");
        const auto frame = Quaternion::CreateFromYawPitchRoll(XMConvertToRadians(-28.5f), 0, 0);
        const auto cast = [&](Vector3 from, Vector3 delta, PhysicsRayHit& hit)
        {
            return physics.RayCastStatic(world, Vector3::Transform(from, frame), Vector3::Transform(delta, frame), hit);
        };
        PhysicsRayHit hit;
        Require(cast({400,200,0},{0,0,100},hit) && std::abs(Vector3::Transform(hit.Position,
            Quaternion::CreateFromYawPitchRoll(XMConvertToRadians(28.5f), 0, 0)).z - 28) < 8,
            "Street-facing wall does not follow the restaurant facade");
        Require(cast({130,300,-1150},{0,200,0},hit) && std::abs(hit.Position.y-402.78f)<.1f &&
            !cast({500,100,-1000},{0,-200,0},hit), "Room slabs filled the exterior notch or missed the rear wing");
        auto& ecs=world.GetECS();
        ecs.ForEach<RigidBodyComponent>([](Entity,const RigidBodyComponent& body){Require(body.Body.IsValid(),"Silently missing Jolt body");});
        size_t convexParts = 0, primitiveParts = 0, staticBoxes = 0, furnitureBodies = 0;
        ecs.ForEach<ColliderComponent, RigidBodyComponent>([&](Entity entity, const ColliderComponent& collider, const RigidBodyComponent& body)
        {
            if (body.MotionType == PhysicsMotionType::Static)
            {
                const auto* compound = std::get_if<CompoundShapeDesc>(&collider.Shape);
                Require(compound && std::all_of(compound->Geometry->Children.begin(), compound->Geometry->Children.end(),
                    [](const auto& child) { return std::holds_alternative<BoxShapeDesc>(child.Shape); }),
                    "Bistro static collision retained a non-box shape");
                staticBoxes += compound->Geometry->Children.size();
                return;
            }
            if (body.MotionType == PhysicsMotionType::Dynamic)
            {
                std::string name = ecs.Get<NameComponent>(entity).value;
                std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (!IsBistroTableware(name)) ++furnitureBodies;
                if (IsBistroTableware(name))
                {
                    const auto* bottle = std::get_if<CompoundShapeDesc>(&collider.Shape);
                    Require(bottle && bottle->Geometry->Children.size() == 1 &&
                        std::holds_alternative<BoxShapeDesc>(bottle->Geometry->Children.front().Shape),
                        "Real Bistro bottle did not use exactly one box");
                }
            }
            Require(!std::holds_alternative<TriangleMeshShapeDesc>(collider.Shape), "Dynamic body received a triangle mesh");
            if (const auto* convex = std::get_if<ConvexCompoundShapeDesc>(&collider.Shape))
            {
                Require(convex->Geometry->Hulls.size() <= 12, "Collider bake exceeded the furniture hull budget");
                convexParts += convex->Geometry->Hulls.size();
                for (const auto& hull : convex->Geometry->Hulls)
                    Require(hull.size() <= 32, "Collider bake exceeded the hull vertex budget");
            }
            else if (const auto* compound = std::get_if<CompoundShapeDesc>(&collider.Shape))
            {
                Require(compound->Geometry->Children.size() <= 12, "Collider bake exceeded its compound part budget");
                for (const auto& child : compound->Geometry->Children)
                {
                    Require(!std::holds_alternative<TriangleMeshShapeDesc>(child.Shape), "Dynamic compound received a triangle mesh");
                    if (const auto* convex = std::get_if<ConvexCompoundShapeDesc>(&child.Shape))
                    {
                        convexParts += convex->Geometry->Hulls.size();
                        for (const auto& hull : convex->Geometry->Hulls)
                            Require(hull.size() <= 32, "Mixed collider exceeded the hull vertex budget");
                    }
                    else ++primitiveParts;
                }
            }
            else ++primitiveParts;
        });
        Require(furnitureBodies == 42, "Count scaling duplicated furniture instead of indoor tableware");
        Require(convexParts == 0, "Bistro dynamics retained expensive convex hull parts");
        const auto walkwayBoxes = BistroRoutes::People().Edges.size();
        Require(staticBoxes > walkwayBoxes + 6 && staticBoxes <= walkwayBoxes + 80,
            "Bistro room and walkway must use small compounds of static boxes");
        Require(primitiveParts <= expected * 12, "Bistro dynamic part budget exceeded");
        std::cout << "Baked colliders: " << staticBoxes - walkwayBoxes << " room boxes, " << walkwayBoxes
            << " walkway boxes, " << primitiveParts << " dynamic primitive parts\n";
        for (const Vector3 point : {Vector3(460.402f,37.0268f,-585.395f),Vector3(614.059f,45.f,-388.402f)})
        {
            PhysicsRayHit inside;
            Require(physics.RayCastStatic(world,point,{0,.1f,0},inside) && Vector3::Distance(inside.Position,point)<.01f,
                "Cooked indoor floor lacks solid volume below its visible surface");
        }
        DynamicBodyUpdateSystem sync(physics);
        Require(waves.ParticipantCount() == expected, "Only spawned props must opt into waves");
        float peakSpeed = 0;
        unsigned step=0;
        const size_t maxTicks=600*(cycles+1);
        std::vector<double> tickTimes;
        tickTimes.reserve(maxTicks);
        double totalPhysicsMs = 0;
        std::vector<Vector3> previousVelocities(expected);
        float minimumScatter=FLT_MAX;
        float minimumLaunch=FLT_MAX;
        std::vector<bool> leftGeometry(centers.size(),false);
        std::vector<bool> belowFloor(centers.size(),false);
        std::vector<unsigned> belowFloorSamples(centers.size(),0);
        size_t outside=0;
        size_t floorPenetrations=0;
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
            const auto tickStart = std::chrono::steady_clock::now();
            physics.Step(world,1.f/60);
            const double tickMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tickStart).count();
            tickTimes.push_back(tickMs);
            totalPhysicsMs += tickMs;
            sync.Schedule(jobs,world,1.f/60).Complete();
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
                if (step%15==0 && !belowFloor[centerIndex])
                {
                    const auto& shape=std::get<CompoundShapeDesc>(ecs.Get<ColliderComponent>(entity).Shape);
                    if (shape.Geometry->Children.size()==1)
                    {
                        const auto bodyCenter=Vector3::Transform(shape.Geometry->Children.front().Position,rotation)+p;
                        PhysicsRayHit support;
                        const bool under=BelowIndoorFloor(bodyCenter,[&](const Vector3& origin,PhysicsRayHit& hit)
                        { return physics.RayCastStatic(world,origin,{0,-30,0},hit); },support);
                        belowFloorSamples[centerIndex]=under ? belowFloorSamples[centerIndex]+1 : 0;
                        if (belowFloorSamples[centerIndex]>=3)
                        {
                            belowFloor[centerIndex]=true; ++floorPenetrations;
                            if (floorPenetrations<=3) std::cout << "Below indoor floor: " << name << " t=" << step/60.
                                << " center=" << bodyCenter.x << ',' << bodyCenter.y << ',' << bodyCenter.z
                                << " floor=" << support.Position.y << '\n';
                        }
                    }
                }
                if ((c.y < 0 || c.y > 423) && !leftGeometry[centerIndex])
                {
                    leftGeometry[centerIndex]=true; ++outside;
                    if(outside<=3) std::cout << "Left open geometry: " << name << " t=" << step/60. << " at=" << c.x << ',' << c.y << ',' << c.z << '\n';
                }
                ++centerIndex;
            }
            if (step%300==0) std::cout << "Progress: " << step << " ticks, " << waves.GetTimeline().CompletedCycles()
                << " waves, mean physics=" << totalPhysicsMs/tickTimes.size() << " ms, escaped=" << outside << '\n';
        }
        Require(waves.GetTimeline().CompletedCycles() == cycles, "Real Bistro did not complete the requested full waves");
#ifdef JPH_ENABLE_ASSERTS
        Require(JoltAssertions.load() == 0, "Jolt asserted during real Bistro collision baking/simulation");
#endif
        size_t moved=0;
        for (const auto& [entity,center,start] : centers)
        {
            const auto& t=ecs.Get<TransformComponent>(entity);
            const auto q=DirectX::SimpleMath::Quaternion::CreateFromYawPitchRoll(DirectX::XMConvertToRadians(t.Rotation.y),DirectX::XMConvertToRadians(t.Rotation.x),DirectX::XMConvertToRadians(t.Rotation.z));
            const auto current=Vector3::Transform(center,q)+t.Location;
            Require(std::isfinite(current.x)&&std::isfinite(current.y)&&std::isfinite(current.z),"Nonfinite physics transform");
            const Vector3 wallProbe(current.x, 300, current.z);
            for (const Vector3 axis : {Vector3::UnitX, -Vector3::UnitX, Vector3::UnitZ, -Vector3::UnitZ})
            {
                PhysicsRayHit wall;
                Require(physics.RayCastStatic(world, wallProbe, Vector3::Transform(axis * 2000, frame), wall),
                    "A dynamic prop ended outside the restaurant walls");
            }
            if (Vector3::Distance(current,start)>1) ++moved;
        }
        scenes->OnWorldDestroyed.Broadcast(world);
        physics.Uninitialize();
        std::sort(tickTimes.begin(), tickTimes.end());
        std::cout << "Physics step: threads=" << jobs.GetThreadCount() << ", mean=" << totalPhysicsMs / tickTimes.size()
            << " ms, p95=" << tickTimes[tickTimes.size() * 95 / 100] << " ms\n";
        Require(outside == 0, "Physical props escaped through the floor or ceiling");
        Require(floorPenetrations == 0, "Tableware penetrated the indoor floor onto the foundation");
        std::cout << "Bistro: " << base << " original objects, " << expected << " dynamic bodies, 2 static bodies, scale=" << scale << ", moved=" << moved << "; " << step << " ticks, " << cycles << " full waves, peak speed=" << peakSpeed << " cm/s, minimum applied scatter=" << minimumScatter << " cm/s, minimum launch=" << minimumLaunch << ", outside modeled geometry=" << outside << " passed\n";
        return 0;
    }
    catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
