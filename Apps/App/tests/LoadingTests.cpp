#include "App.Base/LoadingSettings.h"
#include <cassert>
#include <fstream>
#include <process.h>
#include <iostream>
#include "Engine.Core/IO/TextureLoader.h"
#include "Engine.Core/Jobs/JobSystem.h"

struct DecodeJob final : ParallelJob
{
    std::filesystem::path Path;
    std::vector<std::unique_ptr<Engine::Core::Texture>> Results{ 8 };
    void Execute(size_t index) override { Results[index] = Engine::Core::TextureLoader::LoadTextureAsset(Path); }
};

int main()
{
    const auto configPath = ConsoleModule::GetDefaultConfigPath();
    std::filesystem::create_directories(configPath.parent_path());
    {
        std::ofstream config(configPath);
        config << "[ConsoleVariables]\nLoading.TransientBudgetMiB = 128\n";
        assert(config);
    }
    ConsoleModule{}.Initialize();
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
        assert(LoadingTransientBudgetMiB.GetInt() == 2048);
    }
    assert(!LoadingTransientBudgetMiB.SetValue(0));
    assert(!LoadingTransientBudgetMiB.SetFromString("-1"));
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
    std::cout << "Loading tests: CVar, bounded batches and serial/parallel PNG mip parity passed.\n";
}
