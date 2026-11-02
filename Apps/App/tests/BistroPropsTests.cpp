#include "../src/BistroScene.h"
#include "../src/Components/FlyingPropComponent.h"
#include "App.Base/ECS/Components/StaticMeshRenderComponent.h"
#include "Engine.Core/IO/MeshImporter.h"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

static void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

static void CheckParticleBudgets()
{
    const double original = BistroParticles::CountScale.GetValue();
    Require(BistroParticles::CountScale.SetValue(1), "Default particle scale must be writable");
    World world(WorldDesc{});
    BistroParticles::Create(world, nullptr);
    auto& ecs = world.GetECS();
    Require(ecs.GetPool<BistroParticles::EffectComponent>().Size() == 8, "Bistro must have eight distinct effects");
    std::vector<UINT> baseline;
    ecs.ForEach<ParticleEmitterComponent>([&](Entity, const ParticleEmitterComponent& emitter)
    { baseline.push_back(emitter.GetSettings().MaxParticles); });
    Require(baseline.front() == 62500, "Entrance fountain must own one quarter of the budget");
    const std::pair<double, UINT> cases[] = {{4,1000000}, {4194304.0/250000,4194304},
        {1.25,312500}, {0.6,150000}, {0.5,125000}, {0.000028,7}, {0.000004,1},
        {0.000000001,0}, {0,250000}, {-2,250000}, {1,250000}};
    for (const auto& [scale, budget] : cases)
    {
        Require(BistroParticles::CountScale.SetValue(scale), "Valid particle scale rejected");
        BistroParticles::Update(world);
        UINT sum = 0; size_t index = 0;
        ecs.ForEach<ParticleEmitterComponent>([&](Entity, const ParticleEmitterComponent& emitter)
        {
            const auto& settings = emitter.GetSettings();
            Require(IsValidParticleEmitterSettings(settings), "Scaled emitter exceeds renderer limits");
            if (emitter.IsEnabled())
            {
                sum += settings.MaxParticles;
                Require(emitter.GetBurstTotal() >= settings.MaxParticles, "Live budget must be filled without a warmup");
            }
            if (budget == 1000000) Require(settings.MaxParticles == baseline[index] * 4, "Million budget must scale every effect by four");
            ++index;
        });
        Require(sum == static_cast<UINT>(budget), "Emitter capacities must sum to the exact App budget, including rounding and Off");
    }
    Require(!BistroParticles::CountScale.SetFromString("17") && !BistroParticles::CountScale.SetFromString("nan") &&
        !BistroParticles::CountScale.SetFromString("inf"), "Invalid particle scale accepted");
    ecs.ForEach<NameComponent, ParticleEmitterComponent>([](Entity, const NameComponent& name, const ParticleEmitterComponent& emitter)
    {
        const auto& settings = emitter.GetSettings();
        if (name.value == "BistroPlanterFireflies")
            Require(settings.Geometry == ParticleGeometry::Firefly && settings.SizeStart >= 10 &&
                settings.Blend == ParticleBlend::WeightedAlpha, "Fireflies must use visible, volumetric mesh particles");
        if (name.value == "BistroEntranceFountain") Require(settings.SizeStart >= 7.5f, "Fountain particles must be larger");
    });
    world.SetPaused(true);
    std::vector<uint64_t> bursts;
    ecs.ForEach<ParticleEmitterComponent>([&](Entity, const ParticleEmitterComponent& emitter) { bursts.push_back(emitter.GetBurstTotal()); });
    BistroParticles::Update(world);
    size_t index = 0;
    ecs.ForEach<ParticleEmitterComponent>([&](Entity, const ParticleEmitterComponent& emitter)
    { Require(emitter.GetBurstTotal() == bursts[index++], "Paused world must not queue replenishment"); });
    Require(BistroParticles::CountScale.SetValue(original), "Cannot restore particle scale");
    std::cout << "Bistro particle scales: default, 4x, fractions, <=0 fallback, maximum, rounding, pause and visible mesh settings passed.\n";
}

static void CheckPartition()
{
    std::vector<Engine::Core::SubMesh> parts(7);
    const char* names[] = { "Wall", "Paris_LiquorBottle_1", "Paris_LiquorBottle_1",
        "Paris_Chair_04", "Paris_LiquorBottle_1", "Cooler_02_paris_liquorbottle_3", "Floor" };
    for (size_t i = 0; i < parts.size(); ++i)
    {
        parts[i].SourceNodeName = names[i];
        parts[i].MaterialIndex = static_cast<uint32_t>(i);
    }
    Engine::Core::Mesh mesh(std::move(parts), {});
    const auto partition = PartitionBistroProps(mesh);
    Require(partition.StaticSubMeshes == std::vector<uint32_t>({ 0, 6 }), "Background must exclude flying geometry");
    Require(partition.Props.size() == 3, "Case-insensitive prop names must select logical objects");
    Require(partition.Props[0].SubMeshIndices == std::vector<uint32_t>({ 1, 2, 4 }), "All bottle material sections must move together");
    std::vector<unsigned> visits(mesh.GetSubMeshCount());
    for (auto index : partition.StaticSubMeshes) ++visits.at(index);
    for (const auto& prop : partition.Props)
        for (auto index : prop.SubMeshIndices) ++visits.at(index);
    for (auto count : visits) Require(count == 1, "Every source submesh must belong to exactly one entity");
    Require(mesh.GetSubMesh(4).MaterialIndex == 4, "Partition must retain source material indices");

    StaticMeshRenderComponent whole({}, {});
    StaticMeshRenderComponent selected({}, {}, { 1, 2, 4 });
    Require(whole.GetSubMeshCount(7) == 7 && whole.GetSubMeshIndex(4) == 4, "Existing renderers must retain all submeshes");
    Require(selected.GetSubMeshCount(7) == 3 && selected.GetSubMeshIndex(2) == 4, "Selected renderers must retain original mesh indices");

    World world(WorldDesc{});
    auto& ecs = world.GetECS();
    ecs.RegisterComponent<FlyingPropComponent>();
    CreateBistroPropEntities(world, "Interior_interior", whole, partition);
    Require(ecs.GetPool<FlyingPropComponent>().Size() == 3, "The App must attach flight to each original prop");
    std::vector<unsigned> rendered(mesh.GetSubMeshCount());
    ecs.ForEach<StaticMeshRenderComponent>([&](Entity entity, StaticMeshRenderComponent& renderer)
    {
        Require(renderer.ReflectionDynamic == ecs.Has<FlyingPropComponent>(entity),
            "Only props must opt into the generic dynamic reflection flag");
        for (auto index : renderer.SubMeshIndices) ++rendered.at(index);
    });
    for (auto count : rendered) Require(count == 1, "App setup must not duplicate or drop source geometry");
    ecs.ForEach<FlyingPropComponent>([](Entity, FlyingPropComponent& flight)
    {
        Require(flight.Time == 0 && flight.Speed >= 0.4f, "App setup must preserve initial flight parameters");
    });
}

static void CheckFlight()
{
    FlyingPropComponent flight;
    flight.Phase = 1.4f;
    flight.Speed = 0.7f;
    const auto initial = flight.OffsetAt(0);
    Require(initial.LengthSquared() < 0.000001f, "Flight must start at the original scene position");
    for (unsigned i = 0; i < 1000; ++i)
    {
        const float time = i * 0.17f;
        const auto offset = flight.OffsetAt(time);
        Require(std::isfinite(offset.x) && std::isfinite(offset.y) && std::isfinite(offset.z), "Flight offsets must remain finite");
        Require(std::abs(offset.x) <= 2 * flight.HorizontalRadius + 0.001f &&
            std::abs(offset.z) <= 2 * flight.HorizontalRadius + 0.001f &&
            offset.y >= -0.001f && offset.y <= 2 * flight.VerticalLift + 0.001f, "Flight must stay within its bounded displacement");
        Require((offset - flight.OffsetAt(time)).LengthSquared() == 0, "Flight must be deterministic for the same parameters");
    }
    Require(flight.OffsetAt(1).LengthSquared() > 0.01f, "Flying objects must actually move");
}

static void CheckImporter(const std::filesystem::path& directory)
{
    const auto objPath = directory / "prop-materials.obj";
    {
        std::ofstream material(directory / "prop-materials.mtl");
        material << "newmtl glass\nKd 0.5 0.7 0.8\nnewmtl cap\nKd 0.2 0.2 0.2\n";
        std::ofstream obj(objPath);
        obj << "mtllib prop-materials.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\nv 0 0 1\n"
            "g Paris_LiquorBottle_Test\nusemtl glass\nf 1 2 3\nusemtl cap\nf 1 4 2\n"
            "g Wall\nusemtl cap\nf 2 4 3\n";
    }
    const auto mesh = Engine::Core::MeshImporter::ImportSingleMeshAsset(objPath);
    Require(mesh && mesh->GetSubMeshCount() == 3, "Synthetic OBJ must retain its three material meshes");
    const auto partition = PartitionBistroProps(*mesh);
    Require(partition.Props.size() == 1 && partition.Props[0].Name == "Paris_LiquorBottle_Test",
        "Importer must preserve the owning OBJ node name");
    Require(partition.Props[0].SubMeshIndices.size() == 2 && partition.StaticSubMeshes.size() == 1,
        "Imported material sections must group under their original object");
    Require(mesh->GetSubMesh(partition.Props[0].SubMeshIndices[0]).MaterialIndex !=
        mesh->GetSubMesh(partition.Props[0].SubMeshIndices[1]).MaterialIndex, "Bottle pieces must retain distinct materials");
}

static void CheckInterior(const std::filesystem::path& path)
{
    const auto mesh = Engine::Core::MeshImporter::ImportSingleMeshAsset(path);
    Require(mesh && mesh->GetSubMeshCount() == 2381, "Bistro Interior geometry must remain intact");
    const auto partition = PartitionBistroProps(*mesh);
    Require(partition.Props.size() == 346 && partition.StaticSubMeshes.size() == 1427,
        "Interior must retain its 346 existing props and static remainder");
    std::vector<unsigned> visits(mesh->GetSubMeshCount());
    for (auto index : partition.StaticSubMeshes) ++visits.at(index);
    size_t propSections = 0;
    for (const auto& prop : partition.Props)
        for (auto index : prop.SubMeshIndices) { ++visits.at(index); ++propSections; }
    Require(propSections == 954, "Interior props must retain all their material sections");
    for (auto count : visits) Require(count == 1, "Real Interior geometry must not be lost or duplicated");
    std::cout << "Actual Interior: 346 props, 954 prop submeshes, 1427 static submeshes; exact coverage passed.\n";
}

int main(int argc, char** argv)
{
    try
    {
        Require(argc == 2 || argc == 3, "Pass a test-artifact directory and optionally Bistro Interior OBJ");
        CheckParticleBudgets();
        CheckPartition();
        CheckFlight();
        CheckImporter(argv[1]);
        if (argc == 3) CheckInterior(argv[2]);
        std::cout << "Bistro prop partition, material grouping, selected indices and deterministic flight passed.\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
