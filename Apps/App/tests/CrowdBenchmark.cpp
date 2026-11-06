#define WinMain BistroEntryPointForCrowdBenchmark
#include "../src/App.cpp"
#undef WinMain
#include "Engine.RendererDX12/GDX12GPUTimer.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <numeric>
#include <vector>

namespace
{
    constexpr unsigned Seed = 0x5eed;
    constexpr double Step = 1.0 / 60;

    void Require(bool condition, const char* message)
    {
        if (!condition) throw std::runtime_error(message);
    }

    void CheckFixedTimer()
    {
        GameTimer timer;
        timer.Reset();
        for (int frame = 0; frame < 2; ++frame)
        {
            timer.Tick(Step);
            Require(timer.DeltaTime() == static_cast<float>(Step), "Fixed timer step changed");
        }
        timer.Stop();
        timer.Tick(Step);
        Require(timer.DeltaTime() == 0, "Stopped timer must ignore the fixed step");
        timer.Start();
        timer.Tick();
        Require(timer.DeltaTime() >= 0, "Realtime timer did not resume");
        bool rejected = false;
        try { timer.Tick(std::numeric_limits<double>::quiet_NaN()); }
        catch (const std::invalid_argument&) { rejected = true; }
        Require(rejected, "Non-finite fixed timer step accepted");
    }

    double CPUTimeMS()
    {
        FILETIME creation, exit, kernel, user;
        Require(GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user) != FALSE,
            "Cannot read process CPU time");
        const auto ticks = [](FILETIME value)
        { return (static_cast<std::uint64_t>(value.dwHighDateTime) << 32) | value.dwLowDateTime; };
        return static_cast<double>(ticks(kernel) + ticks(user)) / 10000.0;
    }

    std::uint64_t CrowdChecksum(World& world)
    {
        struct State { Vector3 Position, Rotation; double Time; };
        std::map<std::string, State> states;
        world.GetECS().ForEach<NameComponent, TransformComponent, SkeletalMeshRenderComponent>(
            [&](Entity, const NameComponent& name, const TransformComponent& transform, const SkeletalMeshRenderComponent& mesh)
            { states.emplace(name.value, State{transform.Location, transform.Rotation, mesh.GetTimeSeconds()}); });
        std::uint64_t hash = 14695981039346656037ull;
        const auto append = [&](const void* data, size_t bytes)
        {
            const auto* source = static_cast<const unsigned char*>(data);
            for (size_t i = 0; i < bytes; ++i) { hash ^= source[i]; hash *= 1099511628211ull; }
        };
        for (const auto& [name, state] : states)
        {
            append(name.c_str(), name.size() + 1);
            const float coordinates[] = {state.Position.x, state.Position.y, state.Position.z,
                state.Rotation.x, state.Rotation.y, state.Rotation.z};
            for (const auto value : coordinates) Require(std::isfinite(value), "Non-finite benchmark crowd transform");
            Require(std::isfinite(state.Time), "Non-finite benchmark animation time");
            append(coordinates, sizeof(coordinates));
            append(&state.Time, sizeof(state.Time));
        }
        return hash;
    }

    int FrameCount(const char* argument, bool zeroAllowed)
    {
        size_t consumed = 0;
        const std::string text(argument);
        const int value = std::stoi(text, &consumed);
        Require(consumed == text.size() && value >= (zeroAllowed ? 0 : 1), "Invalid benchmark frame count");
        return value;
    }

    double Percentile(std::vector<double> values, double fraction)
    {
        std::sort(values.begin(), values.end());
        return values[static_cast<size_t>(fraction * (values.size() - 1))];
    }
}

int main(int argc, char** argv)
{
    try
    {
        CheckFixedTimer();
        if (argc == 2 && std::strcmp(argv[1], "--self-check") == 0)
        {
            std::cout << "Crowd benchmark fixed timer self-check passed.\n";
            return 0;
        }
        Require(argc >= 2 && argc <= 4, "Usage: CrowdBenchmark.exe output.json [sample_frames=600] [warmup_frames=120]");
        const int samples = argc >= 3 ? FrameCount(argv[2], false) : 600;
        const int warmup = argc >= 4 ? FrameCount(argv[3], true) : 120;
        ConsoleModule{}.Initialize();
        BistroApp app(GetModuleHandleW(nullptr), Seed);
        Require(app.Initialize(), "Crowd benchmark initialization failed");
        const auto window = app.GetWindow()->GetWindowHandle();
        ShowWindow(window, SW_HIDE);
        app.MsgProc(window, WM_KEYUP, VK_F1, 0);
        BistroBounds::Show.SetFromString("false");
        BistroReflections::ShowBounds.SetFromString("false");
        auto render = BenchmarkEngine::GetLocator().GetModule<RenderModule>();
        auto& world = *BenchmarkEngine::GetLocator().GetModule<SceneManagerModule>()->GetWorld();
        Require(!world.IsPaused() && world.GetTimeScale() == 1, "Crowd benchmark needs an unpaused, unscaled world");
        size_t humans = 0, foxes = 0;
        world.GetECS().ForEach<NameComponent, SkeletalMeshRenderComponent>(
            [&](Entity, const NameComponent& name, const SkeletalMeshRenderComponent&)
            {
                humans += name.value.find("BistroNPC") == 0;
                foxes += name.value.find("BistroFox") == 0;
            });
        Require(humans == static_cast<size_t>(BistroCharacters::NPCCount.GetValue()) &&
            foxes == static_cast<size_t>(BistroCharacters::FoxCount.GetValue()), "Startup benchmark population differs from CVars");
        const auto spawnChecksum = CrowdChecksum(world);
        for (int frame = 0; frame < warmup; ++frame) app.BenchmarkFrame(Step);
        render->WaitForUploads();
        const auto gpuStart = render->GetPrimaryGPUTimingTotals();
        const auto warmChecksum = CrowdChecksum(world);
        std::vector<double> frameTimes, gpuTimes;
        frameTimes.reserve(samples); gpuTimes.reserve(samples);
        double preparationMS = 0;
        const double cpuStart = CPUTimeMS();
        const auto start = std::chrono::steady_clock::now();
        for (int frame = 0; frame < samples; ++frame)
        {
            const auto before = std::chrono::steady_clock::now();
            app.BenchmarkFrame(Step);
            frameTimes.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - before).count());
            preparationMS += render->GetSkeletalPreparationTimeMS();
            const double gpuTime = render->GetPrimaryGPUTimeMS();
            if (gpuTime >= 0) gpuTimes.push_back(gpuTime);
        }
        render->WaitForUploads();
        const double wallMS = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        const double cpuMS = CPUTimeMS() - cpuStart;
        const auto gpuEnd = render->GetPrimaryGPUTimingTotals();
        Require(gpuEnd.Count == gpuStart.Count + static_cast<UINT64>(samples),
            "GPU timer sample count differs from submitted benchmark frames");
        const double gpuMS = gpuEnd.Milliseconds - gpuStart.Milliseconds;
        Require(!gpuTimes.empty(), "No GPU timer values completed; increase warmup/sample frames");
        const auto sampleChecksum = CrowdChecksum(world);
        const auto gpuMemory = render->GetMemoryStatistics();
        PROCESS_MEMORY_COUNTERS_EX memory = {};
        memory.cb = sizeof(memory);
        Require(GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)) != FALSE,
            "Cannot read benchmark process memory");
        const auto* common = render->GetRenderPipelineCommonData();
        const auto& camera = world.GetECS().Get<TransformComponent>(world.ActiveCamera);
        std::ofstream report(argv[1]);
        Require(bool(report), "Cannot open crowd benchmark report");
        report << std::setprecision(12) << "{\n  \"seed\": " << Seed
            << ",\n  \"fixed_delta_seconds\": " << Step
            << ",\n  \"warmup_frames\": " << warmup << ",\n  \"sampled_frames\": " << samples
            << ",\n  \"humans\": " << humans << ",\n  \"foxes\": " << foxes
            << ",\n  \"width\": " << common->WindowWidth << ",\n  \"height\": " << common->WindowHeight
            << ",\n  \"camera_position\": [" << camera.Location.x << ',' << camera.Location.y << ',' << camera.Location.z << ']'
            << ",\n  \"camera_rotation\": [" << camera.Rotation.x << ',' << camera.Rotation.y << ',' << camera.Rotation.z << ']'
            << ",\n  \"wall_ms_per_frame\": " << wallMS / samples
            << ",\n  \"aggregate_cpu_ms_per_frame\": " << cpuMS / samples
            << ",\n  \"skeletal_preparation_wall_ms_per_frame\": " << preparationMS / samples
            << ",\n  \"gpu_timing_sample_count\": " << gpuEnd.Count - gpuStart.Count
            << ",\n  \"gpu_ms_per_frame\": " << gpuMS / samples
            << ",\n  \"frame_submit_wall_ms_median\": " << Percentile(frameTimes, 0.5)
            << ",\n  \"frame_submit_wall_ms_p95\": " << Percentile(frameTimes, 0.95)
            << ",\n  \"gpu_latest_resolved_observations\": " << gpuTimes.size()
            << ",\n  \"gpu_latest_resolved_ms_mean\": " << std::accumulate(gpuTimes.begin(), gpuTimes.end(), 0.0) / gpuTimes.size()
            << ",\n  \"gpu_latest_resolved_ms_median\": " << Percentile(gpuTimes, 0.5)
            << ",\n  \"gpu_latest_resolved_ms_p95\": " << Percentile(gpuTimes, 0.95)
            << ",\n  \"gpu_sample_kind\": \"Latest completed buffered timer value after each submitted frame; observations may repeat or omit GPU frames\""
            << ",\n  \"gpu_allocation_bytes\": " << gpuMemory.AllocationBytes
            << ",\n  \"gpu_allocation_count\": " << gpuMemory.AllocationCount
            << ",\n  \"gpu_block_bytes\": " << gpuMemory.BlockBytes
            << ",\n  \"working_set_bytes\": " << memory.WorkingSetSize
            << ",\n  \"private_bytes\": " << memory.PrivateUsage
            << ",\n  \"spawn_checksum\": \"" << spawnChecksum << "\""
            << ",\n  \"warm_checksum\": \"" << warmChecksum << "\""
            << ",\n  \"sample_checksum\": \"" << sampleChecksum << "\""
            << ",\n  \"checksum_kind\": \"FNV1a of name-sorted crowd names, position/rotation float bytes and animation-time double bytes\""
            << ",\n  \"settings\": {";
        const wchar_t* settings[] = {L"Render.Skinning.Enabled", L"Render.Skinning.Quality", L"Render.Skinning.Instancing",
            L"Render.Upscaler.Provider", L"Render.FrameGeneration.Provider", L"Render.AO.Enabled", L"Render.SSR.Enabled",
            L"Render.Reflection.Enabled", L"Render.Particles.Enabled", L"Render.Grass.Enabled", L"Render.Fluid.Enabled",
            L"Render.NumFrames", L"Jobs.ThreadCount", L"App.ReflectionProbes.Count", L"App.Grass.Count", L"App.Water.Count", L"App.Particles.Count"};
        for (size_t index = 0; index < std::size(settings); ++index)
        {
            IConsoleVariable* variable = nullptr;
            const bool found = ConsoleModule{}.TryFindConsoleVariable(settings[index], variable);
            report << (index ? "," : "") << "\n    \"" << WStringToString(settings[index]) << "\": \""
                << (found ? variable->GetString() : "unavailable") << '\"';
        }
        report << "\n  }\n}\n";
        report.close();
        Require(bool(report), "Cannot write crowd benchmark report");
        std::cout << "Crowd benchmark: " << humans << " humans, " << foxes << " foxes; wall " << wallMS / samples
            << " ms/frame; aggregate CPU " << cpuMS / samples << " ms/frame; checksum " << sampleChecksum << '\n';
        return 0;
    }
    catch (const DxException& error) { std::wcerr << error.ToString() << '\n'; return 1; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
