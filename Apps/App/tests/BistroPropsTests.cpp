#include "App.Base/BistroProps.h"
#include "App.Base/ECS/Components/FlyingPropComponent.h"
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
        CheckPartition();
        CheckFlight();
        CheckImporter(argv[1]);
        if (argc == 3) CheckInterior(argv[2]);
        std::cout << "Bistro prop partition, material grouping, selected indices and deterministic flight passed.\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
