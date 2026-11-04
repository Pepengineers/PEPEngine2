#include "../src/BistroProps.h"
#include "App.Base/Physics/JoltPhysicsBackend.h"
#include "App.Base/Modules/SceneManagerModule.h"
#include "App.Base/Systems/DynamicBodyUpdateSystem.h"
#include "Engine.Core/BenchmarkEngine.h"
#include "Engine.Core/IO/MeshImporter.h"
#include "Engine.Core/Jobs/JobSystem.h"
#include <iostream>
#include <limits>

static void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
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
        const double scale = argc > 1 ? std::stod(argv[1]) : 1;
        Require(BistroPhysics::CountScale.SetValue(scale), "Invalid test scale");
        JobSystem jobs(argc > 2 ? static_cast<unsigned>(std::stoul(argv[2])) : 4);
        CheckBakedPivot(jobs);
        BenchmarkEngine engine;
        auto scenes = std::make_shared<SceneManagerModule>();
        BenchmarkEngine::GetLocator().RegisterModule(scenes);
        PhysicsModule physics(jobs);
        physics.Initialize();
        World world(WorldDesc{});
        scenes->OnWorldCreated.Broadcast(world);
        physics.ConfigureWorld(world, BistroPhysics::Settings());
        size_t base = 0, expected = 0;
        std::vector<std::pair<Entity,Vector3>> centers;
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
            CreateBistroPropEntities(world, part, StaticMeshRenderComponent({},{}), *mesh, partition, &physics);
            auto& ecs = world.GetECS();
            ecs.ForEach<NameComponent,StaticMeshRenderComponent,RigidBodyComponent>([&](Entity entity, const NameComponent& name, const StaticMeshRenderComponent& render, const RigidBodyComponent& body)
            {
                if (name.value.find("_PhysicsCopy_") != std::string::npos || body.MotionType != PhysicsMotionType::Dynamic) return;
                if (std::find_if(centers.begin(),centers.end(),[&](const auto& v){return v.first==entity;}) != centers.end()) return;
                centers.emplace_back(entity,Vector3(BistroPhysics::Bounds(*mesh,render.SubMeshIndices).Center));
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
        for (int step=0; step<300; ++step) { physics.Step(world,1.f/60); sync.Schedule(jobs,world,1.f/60).Complete(); }
        size_t moved=0;
        for (const auto& [entity,center] : centers)
        {
            const auto& t=ecs.Get<TransformComponent>(entity);
            const auto q=DirectX::SimpleMath::Quaternion::CreateFromYawPitchRoll(DirectX::XMConvertToRadians(t.Rotation.y),DirectX::XMConvertToRadians(t.Rotation.x),DirectX::XMConvertToRadians(t.Rotation.z));
            const auto current=Vector3::Transform(center,q)+t.Location;
            Require(std::isfinite(current.x)&&std::isfinite(current.y)&&std::isfinite(current.z),"Nonfinite physics transform");
            Require(current.y > -1000 && Vector3::Distance(current,center)<3000,"Original item escaped scene or has wrong pivot");
            if (Vector3::Distance(current,center)>1) ++moved;
        }
        scenes->OnWorldDestroyed.Broadcast(world);
        physics.Uninitialize();
        std::cout << "Bistro: " << base << " original objects, " << expected << " dynamic bodies, 2 static meshes, scale=" << scale << ", moved=" << moved << "; 300 ticks passed\n";
        return 0;
    }
    catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
