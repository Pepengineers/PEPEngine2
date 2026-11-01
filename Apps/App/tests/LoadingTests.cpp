#include "App.Base/LoadingSettings.h"
#include "Engine.Core/ConsoleModule.h"
#include <limits>
#include <cassert>
#include <fstream>
#include <process.h>
#include <iostream>
#include "Engine.Core/IO/TextureLoader.h"
#include "Engine.Core/Jobs/JobSystem.h"
#include "Common/GameTimer.h"
#include "Engine.Core/IO/MeshImporter.h"
#include "Engine.Core/Registries/TextureRegistry.h"
#include "Engine.Core/IO/AssetCache.h"
#include "Engine.Core/IO/MeshCache.h"
#include <directxtex/DirectXTex.h>

struct DecodeJob final : ParallelJob
{
    std::filesystem::path Path;
    std::vector<std::unique_ptr<Engine::Core::Texture>> Results{ 8 };
    void Execute(size_t index) override { Results[index] = Engine::Core::TextureLoader::LoadTextureAsset(Path); }
};

static void CheckMeshCache(const std::filesystem::path& path)
{
    using namespace Engine::Core;
    SubMesh part;
    part.SourceNodeName = u8"test node / геометрия";
    part.Bounds = {{1, 2, 3}, {4, 5, 6}};
    part.Vertices.resize(3);
    part.Vertices[0].Position = {1, 2, 3};
    part.Vertices[0].Normal = {0, 0, 1};
    part.Vertices[0].TexCoord = {0.25f, 0.75f};
    part.Vertices[0].Tangent = {0, 1, 0};
    part.Vertices[0].PreviousPosition = {3, 2, 1};
    part.Indices = {0, 2, 1};
    part.Bones.push_back({"bone", DirectX::SimpleMath::Matrix::CreateTranslation(1, 2, 3)});
    part.SkinVertices = {{0, 1}, {1, 1}, {2, 1}};
    part.Influences = {{0, 1}, {0, 1}, {0, 1}};
    MeshMaterial material;
    material.Name = u8"материал";
    material.DiffuseTexturePath = L"D:/textures/цвет.png";
    material.NormalTexturePath = L"normal.png";
    material.SpecularTexturePath = L"specular.png";
    material.RoughnessTexturePath = L"roughness.png";
    material.EmissiveTexturePath = L"emissive.png";
    material.OpacityTexturePath = L"opacity.png";
    material.DiffuseColor = {0.1f, 0.2f, 0.3f};
    material.SpecularColor = {0.4f, 0.5f, 0.6f};
    material.EmissiveColor = {1, 2, 4};
    material.Roughness = 0.25f; material.Metallic = 0.75f; material.Opacity = 0.5f;
    material.Type = EMaterialType::Transparent; material.UseBakedLighting = true;
    Mesh source({part}, part.Bounds, {material});
    const auto check = [&](const Mesh& mesh)
    {
        assert(mesh.GetSubMeshCount() == 1 && mesh.GetMaterials().size() == 1);
        assert(std::memcmp(&mesh.GetBounds(), &source.GetBounds(), sizeof(DirectX::BoundingBox)) == 0);
        const auto& actual = mesh.GetSubMesh(0);
        assert(actual.MaterialIndex == part.MaterialIndex && actual.SourceNodeName == part.SourceNodeName);
        assert(std::memcmp(&actual.Bounds, &part.Bounds, sizeof(part.Bounds)) == 0);
        assert(actual.Vertices.size() == part.Vertices.size() && actual.Indices == part.Indices);
        assert(std::memcmp(actual.Vertices.data(), part.Vertices.data(), part.Vertices.size() * sizeof(Vertex)) == 0);
        assert(actual.Bones.size() == 1 && actual.Bones[0].NodeName == part.Bones[0].NodeName);
        assert(actual.Bones[0].InverseBind == part.Bones[0].InverseBind);
        assert(actual.SkinVertices.size() == part.SkinVertices.size() && actual.Influences.size() == part.Influences.size());
        assert(std::memcmp(actual.SkinVertices.data(), part.SkinVertices.data(), part.SkinVertices.size() * sizeof(SkinVertexRange)) == 0);
        assert(std::memcmp(actual.Influences.data(), part.Influences.data(), part.Influences.size() * sizeof(SkinInfluence)) == 0);
        const auto& m = mesh.GetMaterial(0);
        assert(m.Name == material.Name && m.DiffuseTexturePath == material.DiffuseTexturePath &&
            m.NormalTexturePath == material.NormalTexturePath && m.SpecularTexturePath == material.SpecularTexturePath &&
            m.RoughnessTexturePath == material.RoughnessTexturePath && m.EmissiveTexturePath == material.EmissiveTexturePath &&
            m.OpacityTexturePath == material.OpacityTexturePath);
        assert(m.DiffuseColor == material.DiffuseColor && m.SpecularColor == material.SpecularColor && m.EmissiveColor == material.EmissiveColor);
        assert(m.Roughness == material.Roughness && m.Metallic == material.Metallic && m.Opacity == material.Opacity &&
            m.Type == material.Type && m.UseBakedLighting == material.UseBakedLighting);
    };
    assert(SaveMeshCache(path, source));
    const auto loaded = LoadMeshCache(path);
    assert(loaded); check(*loaded);
    assert(!LoadMeshCache(path, [] { return false; }));
    assert(!SaveMeshCache(path, source, [] { return false; }));
    assert(!std::filesystem::exists(CacheTemporaryPath(path)));
    check(*LoadMeshCache(path));
    // Invalid counts must be rejected before allocation, even with a large declared count.
    for (const auto offset : {8, 40})
    {
        { std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
          file.seekp(offset); const uint32_t invalid = UINT32_MAX; file.write(reinterpret_cast<const char*>(&invalid), sizeof(invalid)); }
        assert(!LoadMeshCache(path));
        assert(SaveMeshCache(path, source));
    }
    // A structurally valid changed float still has to fail the checksum.
    { std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary); file.seekp(16); const float changed = 123;
      file.write(reinterpret_cast<const char*>(&changed), sizeof(changed)); }
    assert(!LoadMeshCache(path));
    assert(SaveMeshCache(path, source));
    std::filesystem::resize_file(path, std::filesystem::file_size(path) - 1);
    assert(!LoadMeshCache(path));
    part.Indices[0] = 99;
    assert(SaveMeshCache(path, Mesh({part}, part.Bounds, {material})));
    assert(!LoadMeshCache(path));
    std::filesystem::remove(path);
}

int main()
{
    GameTimer timer;
    timer.Reset();
    timer.Stop();
    Sleep(20);
    timer.Start();
    timer.Tick();
    timer.Reset();
    assert(timer.TotalTime() == 0 && timer.DeltaTime() == 0);

    const auto configPath = ConsoleModule::GetDefaultConfigPath();
    std::filesystem::create_directories(configPath.parent_path());
    {
        std::ofstream config(configPath);
        config << "[ConsoleVariables]\nLoading.TransientBudgetMiB = 128\n";
        assert(config);
    }
    ConsoleModule{}.Initialize();
    auto& budget = ConsoleModule{}.FindConsoleVariable(L"Loading.TransientBudgetMiB");
    assert(budget.GetInt() == 128);
    assert(GetLoadingBudgetBytes() == 128ull * 1024 * 1024);
    const std::vector<std::uint64_t> sizes = { 40, 60, 101, 10 };
    assert(LoadingBatchEnd(sizes, 0, 100, 8) == 2);
    assert(LoadingBatchEnd(sizes, 0, 100, 1) == 1);
    assert(LoadingBatchEnd(sizes, 2, 100, 8) == 3);
    assert(LoadingBatchEnd(sizes, 3, 100, 8) == 4);
    assert(LoadingBatchEnd(sizes, 4, 100, 8) == 4);
    assert(!ApplyLoadingArgument(L"-Other=1"));
    assert(ApplyLoadingArgument(L"-Loading.TransientBudgetMiB=2048"));
    assert(GetLoadingBudgetBytes() == 2048ull * 1024 * 1024);
    for (const auto* invalid : { L"", L"0", L"-1", L"1x", L"2147483648", L"99999999999999999999" })
    {
        bool rejected = false;
        try { ApplyLoadingArgument(std::wstring(L"-Loading.TransientBudgetMiB=") + invalid); }
        catch (const std::runtime_error&) { rejected = true; }
        assert(rejected);
        assert(budget.GetInt() == 2048);
    }
    assert(!budget.SetFromString("0"));
    assert(!budget.SetFromString("-1"));
    assert(GetLoadingBudgetBytes() == 2048ull * 1024 * 1024);

    const auto path = std::filesystem::temp_directory_path() / ("PEPEngineLoadingTest-" + std::to_string(_getpid()) + ".png");
    const unsigned char png[] = { 137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,0,2,0,0,0,2,8,6,0,0,0,114,182,13,36,0,0,0,19,73,68,65,84,120,156,99,248,207,192,240,31,12,25,24,254,131,1,0,73,200,9,247,20,99,50,157,0,0,0,0,73,69,78,68,174,66,96,130 };
    {
        std::ofstream image(path, std::ios::binary);
        image.write(reinterpret_cast<const char*>(png), sizeof(png));
        assert(image);
    }
    using Engine::Core::TextureLoader;
    const auto serial = TextureLoader::LoadTextureAsset(path);
    assert(serial && serial->GetWidth() == 2 && serial->GetHeight() == 2 && serial->GetMipLevels() == 2);
    assert(serial->GetType() == Engine::Core::ETextureType::Unknown);
    assert(TextureLoader::EstimateTransientBytes(path) == 128);
    Engine::Core::TextureRegistry registry;
    const auto textureHandle = registry.Register(path);
    assert(registry.Cache(textureHandle, TextureLoader::LoadTextureAsset(path)));
    registry.Unload(textureHandle);
    assert(!registry.GetTexture(textureHandle));
    assert(registry.FindHandle(path) == textureHandle);
    assert(registry.Load(path));
    JobSystem jobs(2);
    DecodeJob decode;
    decode.Path = path;
    decode.Schedule(jobs, decode.Results.size(), 1).Complete();
    for (const auto& result : decode.Results)
    {
        assert(result && result->GetSubresourceCount() == serial->GetSubresourceCount());
        for (std::uint32_t mip = 0; mip < serial->GetMipLevels(); ++mip)
        {
            assert(result->GetSubresource(mip, 0).Data == serial->GetSubresource(mip, 0).Data);
            assert(result->GetSubresource(mip, 0).RowPitch == serial->GetSubresource(mip, 0).RowPitch);
        }
    }
    std::filesystem::remove(path);
    assert(TextureLoader::EstimateTransientBytes(path) == (std::numeric_limits<std::uint64_t>::max)());
    const auto meshPath = path.parent_path() / (path.stem().string() + ".obj");
    CheckMeshCache(path.parent_path() / (path.stem().string() + ".pepmesh"));
    {
        std::ofstream mesh(meshPath);
        mesh << "v 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\nvt 1 0\nvt 0 1\nf 1/1 2/2 3/3\n";
    }
    const auto reference = Engine::Core::MeshImporter::ImportSingleMeshAsset(meshPath);
    Engine::Core::MeshImportOptions options;
    options.bGenerateTangents = false;
    const auto derivativeNormals = Engine::Core::MeshImporter::ImportSingleMeshAsset(meshPath, options);
    assert(reference && derivativeNormals);
    const auto& before = reference->GetSubMesh(0);
    const auto& after = derivativeNormals->GetSubMesh(0);
    assert(before.Indices == after.Indices && before.Vertices.size() == after.Vertices.size());
    for (size_t i = 0; i < before.Vertices.size(); ++i)
    {
        assert(before.Vertices[i].Position == after.Vertices[i].Position);
        assert(before.Vertices[i].Normal == after.Vertices[i].Normal);
        assert(before.Vertices[i].TexCoord == after.Vertices[i].TexCoord);
    }
    options.bCache = true;
    const auto cachedMesh = Engine::Core::MeshImporter::ImportSingleMeshAsset(meshPath, options);
    const auto warmMesh = Engine::Core::MeshImporter::ImportSingleMeshAsset(meshPath, options);
    assert(cachedMesh && warmMesh && warmMesh->GetSubMesh(0).Indices == after.Indices);
    assert(warmMesh->GetSubMesh(0).Vertices[1].Position == after.Vertices[1].Position);
    const auto oldKey = Engine::Core::AssetCachePath(meshPath, "test", true);
    {
        std::ofstream mesh(meshPath, std::ios::app);
        mesh << "# edited source\n";
    }
    assert(Engine::Core::AssetCachePath(meshPath, "test", true) != oldKey);
    auto materialPath = meshPath;
    materialPath.replace_extension(L".mtl");
    { std::ofstream material(materialPath); material << "newmtl sample\nKd 1 0 0\n"; }
    { std::ofstream mesh(meshPath, std::ios::app); mesh << "mtllib\t" << materialPath.filename().string() << '\n'; }
    const auto materialKey = Engine::Core::AssetCachePath(meshPath, "test", true);
    assert(Engine::Core::ObjMaterialLibraries(meshPath).size() == 1);
    { std::ofstream material(materialPath, std::ios::app); material << "d 0.5\n"; }
    assert(Engine::Core::AssetCachePath(meshPath, "test", true) != materialKey);
    {
        std::ofstream mesh(meshPath);
        mesh << "mtllib " << materialPath.filename().string() << "\nusemtl sample\n"
            "v 0 0 0\nv 20 0 0\nv 0 1 0\nf 1 2 3\n";
    }
    const auto edited = Engine::Core::MeshImporter::ImportSingleMeshAsset(meshPath, options);
    const auto editedWarm = Engine::Core::MeshImporter::ImportSingleMeshAsset(meshPath, options);
    assert(edited && editedWarm && editedWarm->GetBounds().Extents.x == 10);
    const auto materialIndex = editedWarm->GetSubMesh(0).MaterialIndex;
    assert(editedWarm->GetMaterial(materialIndex).DiffuseColor.x == 1);
    { std::ofstream material(materialPath); material << "newmtl sample\nKd 0 1 0\nd 0.25\n"; }
    const auto changedMaterial = Engine::Core::MeshImporter::ImportSingleMeshAsset(meshPath, options);
    assert(changedMaterial && changedMaterial->GetMaterial(materialIndex).DiffuseColor.y == 1);
    assert(changedMaterial->GetMaterial(materialIndex).Opacity == 0.25f);
    assert(changedMaterial->GetMaterial(materialIndex).Type == Engine::Core::EMaterialType::Transparent);
    options.Continue = [] { return false; };
    assert(!Engine::Core::MeshImporter::ImportSingleMeshAsset(meshPath, options));
    std::filesystem::remove(meshPath);
    std::filesystem::remove(materialPath);
    auto ddsPath = path;
    ddsPath.replace_extension(L".dds");
    DirectX::ScratchImage input;
    assert(SUCCEEDED(input.Initialize2D(DXGI_FORMAT_R8G8B8A8_UNORM, 8, 8, 1, 1)));
    std::memset(input.GetPixels(), 255, input.GetPixelsSize());
    assert(SUCCEEDED(DirectX::SaveToDDSFile(input.GetImages(), input.GetImageCount(), input.GetMetadata(), DirectX::DDS_FLAGS_NONE, ddsPath.c_str())));
    const auto compressed = TextureLoader::LoadTextureAsset(ddsPath, true);
    assert(compressed && compressed->GetFormat() == DXGI_FORMAT_BC7_UNORM && compressed->GetMipLevels() == 4);
    const auto warm = TextureLoader::LoadTextureAsset(ddsPath, true);
    assert(warm && warm->GetSubresource(0, 0).Data == compressed->GetSubresource(0, 0).Data);
    const auto& block = warm->GetSubresource(0, 0);
    DirectX::Image bcImage{ block.Width, block.Height, warm->GetFormat(), block.RowPitch, block.SlicePitch,
        reinterpret_cast<uint8_t*>(const_cast<std::byte*>(block.Data.data())) };
    DirectX::ScratchImage decoded;
    assert(SUCCEEDED(DirectX::Decompress(bcImage, DXGI_FORMAT_R8G8B8A8_UNORM, decoded)));
    for (size_t i = 0; i < decoded.GetPixelsSize(); ++i) assert(decoded.GetPixels()[i] >= 250);
    const auto cachePath = Engine::Core::AssetCachePath(ddsPath, "bc7-v1.dds");
    { std::ofstream corrupt(cachePath, std::ios::binary | std::ios::trunc); corrupt << "bad"; }
    const auto recovered = TextureLoader::LoadTextureAsset(ddsPath, true);
    assert(recovered && recovered->GetFormat() == DXGI_FORMAT_BC7_UNORM);
    std::filesystem::remove(ddsPath);
    assert(!TextureLoader::LoadTextureAsset(ddsPath, true));
    std::filesystem::remove(cachePath);
    std::cout << "Loading tests: native Mesh roundtrip/integrity/cancellation, timer, texture lifetime, import parity, cache invalidation/recovery, BC7 pixels, CVar and PNG mips passed.\n";
}
