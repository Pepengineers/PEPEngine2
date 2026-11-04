#include <App.Base/App.h>
#include "Engine.UI/UI_test.h"
#include "Engine.Core/ConsoleModule.h"
#include "Engine.Core/Logging/Logger.h"
#include <cmath>
#include <cstdlib>
#include <stdexcept>
#include <chrono>
#include <fstream>
#include <psapi.h>
#include <shellapi.h>
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "psapi.lib")
#include "Systems/FlyingPropSystem.h"
#include "BistroScene.h"
#include "BistroReflections.h"
#include "BistroGrass.h"
#include "BistroWater.h"
#include "BistroCharacters.h"
#include "BistroBounds.h"
#include "App.Base/LoadingSettings.h"

namespace
{
    class BistroApp final : public App
    {
    public:
        explicit BistroApp(const HINSTANCE instance) : App(instance) {}

        bool Initialize() override
        {
            if (!App::Initialize()) return false;
            auto& world = *Locator.GetModule<SceneManagerModule>()->GetWorld();
            UpdateSceneAdditions(world, 0);
            _bounds = std::make_unique<BistroBounds::Scene>(world);
            _bounds->UpdateEmitters(world);
            BistroParticles::Update(world);
            auto& camera = world.GetECS().Get<TransformComponent>(world.ActiveCamera);
            // Start at eye height just outside the main entrance, facing the interior.
            camera.Location = Vector3(-300.0f, 200.0f, 20.0f);
            camera.Rotation = Vector3(0.0f, 117.0f, 0.0f);
            camera.DirtyFlag = true;
            world.GetECS().Get<CameraComponent>(world.ActiveCamera).InvalidateHistory();
            return true;
        }

        void BenchmarkFrame()
        {
            MSG message;
            while (PeekMessage(&message, nullptr, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&message);
                DispatchMessage(&message);
            }
            if (!_benchmarkStarted) { Timer.Reset(); _benchmarkStarted = true; }
            Timer.Start();
            Timer.Tick();
            Update(Timer);
            Render(Timer);
        }

    protected:
        bool AddModules() override
        {
            const auto scenePath = std::filesystem::path(ASSETS_FOLDER) / "Scenes" / "Amazon Lumberyard Bistro";
            for (const auto* part : { L"Exterior/exterior.obj", L"Interior/interior.obj" })
            {
                if (!std::filesystem::is_regular_file(scenePath / part))
                {
                    throw std::runtime_error("Missing Bistro scene part: " + (scenePath / part).string());
                }
            }

            Locator.RegisterModule(std::make_shared<RenderModule>(GetWindow(), &Timer));
            auto scenes = std::make_shared<SceneManagerModule>(scenePath);
            ConfigureBistroProps(*scenes);
            Locator.RegisterModule(scenes);
            return BenchmarkEngine::AddModules();
        }

        void Update(const GameTimer& timer) override
        {
            GDX12_SCOPED_CPU_EVENT("Game update");
            Locator.GetModule<RenderModule>()->BeginFrame();
            OnKeyboardInput(timer);
            Locator.GetModule<RenderModule>()->Update();
            auto& world = *Locator.GetModule<SceneManagerModule>()->GetWorld();
            UpdateSceneAdditions(world, timer.DeltaTime());
            _bounds->UpdateEmitters(world);
            BistroParticles::Update(world);
            if (world.IsPaused())
            {
                _gpuData.Schedule(_jobs, world, 0).Complete();
                _reflections.DrawBounds(world, *Locator.GetModule<RenderModule>());
                _bounds->DrawBounds(*Locator.GetModule<RenderModule>());
                return;
            }
            const float dt = timer.DeltaTime();
            JobHandle work;
            try
            {
                work = _movement.Schedule(_jobs, world, dt);
                work = _circle.Schedule(_jobs, world, dt, work);
                work = _spline.Schedule(_jobs, world, dt, work);
                work = _flyingProps.Schedule(_jobs, world, dt, work);
                work = _lookAt.Schedule(_jobs, world, dt, work);
                work = _lights.Schedule(_jobs, world, dt, work);
                work.Complete();
                UpdatePhysics(world, dt);
                work = _gpuData.Schedule(_jobs, world, dt);
                work.Complete();
                _reflections.DrawBounds(world, *Locator.GetModule<RenderModule>());
                _bounds->DrawBounds(*Locator.GetModule<RenderModule>());
            }
            catch (...)
            {
                try { work.Complete(); } catch (...) {}
                throw;
            }
        }

        void OnMouseDown(WPARAM, int x, int y) override
        {
            _lastMousePos = { x, y };
            SetCapture(GetWindow()->GetWindowHandle());
        }

        void OnMouseUp(WPARAM, int, int) override
        {
            ReleaseCapture();
        }

        void OnMouseMove(WPARAM buttons, int x, int y) override
        {
            if (buttons & MK_RBUTTON)
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
            _cameraSpeed = std::clamp(_cameraSpeed * (GET_WHEEL_DELTA_WPARAM(rotation) > 0 ? 1.25f : 0.8f), 0.1f, 10000.0f);
        }

        void OnKeyboardInput(const GameTimer& timer) override
        {
            if (bInactive || Engine::UI::WantsKeyboardInput()) return;
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

    private:
        void UpdateSceneAdditions(World& world, float deltaSeconds)
        {
            _reflections.Update(world, deltaSeconds);
            _grass.Update(world, deltaSeconds);
            _water.Update(world, deltaSeconds);
            _characters.Update(world, deltaSeconds, _jobs);
        }

        BistroReflections::Scene _reflections;
        BistroGrass::Scene _grass;
        BistroWater::Scene _water;
        BistroCharacters::Scene _characters;
        std::unique_ptr<BistroBounds::Scene> _bounds;
        bool _benchmarkStarted = false;
        FlyingPropSystem _flyingProps;
        POINT _lastMousePos = {};
        float _cameraSpeed = 100.0f;
    };
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, PSTR, int)
{
#if defined(DEBUG) || defined(_DEBUG)
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif
    try
    {
        ConsoleModule{}.Initialize();
        std::filesystem::path benchmarkOutput;
        int benchmarkFrames = 1;
        std::wstring budgetArgument;
        int argumentCount = 0;
        auto arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
        if (!arguments) throw std::runtime_error("Cannot read startup arguments");
        for (int index = 1; index < argumentCount; ++index)
        {
            const std::wstring_view argument(arguments[index]);
            constexpr std::wstring_view particlesPrefix = L"-App.Particles.CountScale=";
            if (argument.substr(0, particlesPrefix.size()) == particlesPrefix &&
                !BistroParticles::CountScale.SetFromString(WStringToString(std::wstring(argument.substr(particlesPrefix.size())))))
            {
                Log::Error("Application", "App.Particles.CountScale must be finite and at most 4194304/250000");
                Log::Flush();
                LocalFree(arguments);
                return EXIT_FAILURE;
            }
            constexpr std::wstring_view budgetPrefix = L"-Loading.TransientBudgetMiB=";
            if (argument.substr(0, budgetPrefix.size()) == budgetPrefix) budgetArgument = argument;
            constexpr std::wstring_view prefix = L"-Loading.BenchmarkOutput=";
            if (argument.substr(0, prefix.size()) == prefix)
                benchmarkOutput = std::wstring(argument.substr(prefix.size()));
            constexpr std::wstring_view framesPrefix = L"-Loading.BenchmarkFrames=";
            if (argument.substr(0, framesPrefix.size()) == framesPrefix)
            {
                const std::wstring value(argument.substr(framesPrefix.size()));
                size_t consumed = 0;
                benchmarkFrames = std::stoi(value, &consumed);
                if (consumed != value.size() || benchmarkFrames < 1 || benchmarkFrames > 10000)
                    throw std::runtime_error("Loading.BenchmarkFrames must be between 1 and 10000");
            }
        }
        LocalFree(arguments);
        const auto started = std::chrono::steady_clock::now();
        BistroApp app(instance);
        if (!budgetArgument.empty()) ApplyLoadingArgument(budgetArgument);
        if (!app.Initialize()) return EXIT_FAILURE;
        const auto initialized = std::chrono::steady_clock::now();
        if (!benchmarkOutput.empty())
        {
            app.BenchmarkFrame();
            BenchmarkEngine::GetLocator().GetModule<RenderModule>()->WaitForUploads();
            const auto ready = std::chrono::steady_clock::now();
            for (int frame = 1; frame < benchmarkFrames; ++frame) app.BenchmarkFrame();
            BenchmarkEngine::GetLocator().GetModule<RenderModule>()->WaitForUploads();
            const auto sampled = std::chrono::steady_clock::now();
            auto renderer = BenchmarkEngine::GetLocator().GetModule<RenderModule>();
            const auto gpuMemory = renderer->GetMemoryStatistics();
            const auto graph = renderer->GetRenderGraphStatistics();
            std::ofstream graphOutput(benchmarkOutput.wstring() + L".dot");
            renderer->WriteRenderGraph(graphOutput);
            PROCESS_MEMORY_COUNTERS_EX memory = {};
            memory.cb = sizeof(memory);
            if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)))
                throw std::runtime_error("Cannot read process memory counters");
            std::ofstream report(benchmarkOutput);
            if (!report) throw std::runtime_error("Cannot open loading benchmark output");
            report << "{\n  \"initialize_seconds\": " << std::chrono::duration<double>(initialized - started).count()
                << ",\n  \"ready_seconds\": " << std::chrono::duration<double>(ready - started).count()
                << ",\n  \"budget_mib\": " << GetLoadingBudgetBytes() / (1024u * 1024u)
                << ",\n  \"import_seconds\": " << LastLoadingMeasurements.ImportSeconds
                << ",\n  \"texture_seconds\": " << LastLoadingMeasurements.TextureSeconds
                << ",\n  \"publish_seconds\": " << LastLoadingMeasurements.PublishSeconds
                << ",\n  \"prepared_textures\": " << LastLoadingMeasurements.TextureCount
                << ",\n  \"max_import_tasks\": " << LastLoadingMeasurements.MaxImportTasks
                << ",\n  \"max_texture_tasks\": " << LastLoadingMeasurements.MaxTextureTasks
                << ",\n  \"sampled_frames\": " << benchmarkFrames - 1
                << ",\n  \"particle_count_scale\": " << BistroParticles::CountScale.GetValue()
                << ",\n  \"particle_budget\": " << BistroParticles::GetBudget()
                << ",\n  \"frame_ms\": " << (benchmarkFrames > 1 ? std::chrono::duration<double, std::milli>(sampled - ready).count() / (benchmarkFrames - 1) : 0)
                << ",\n  \"graph_passes\": " << graph.Passes
                << ",\n  \"graph_culled_passes\": " << graph.CulledPasses
                << ",\n  \"graph_logical_textures\": " << graph.TransientTextures
                << ",\n  \"graph_physical_textures\": " << graph.PhysicalTextures
                << ",\n  \"graph_logical_bytes\": " << graph.LogicalBytes
                << ",\n  \"graph_physical_bytes\": " << graph.PhysicalBytes
                << ",\n  \"gpu_allocation_count\": " << gpuMemory.AllocationCount
                << ",\n  \"gpu_block_count\": " << gpuMemory.BlockCount
                << ",\n  \"gpu_allocation_bytes\": " << gpuMemory.AllocationBytes
                << ",\n  \"gpu_block_bytes\": " << gpuMemory.BlockBytes
                << ",\n  \"working_set_bytes\": " << memory.WorkingSetSize
                << ",\n  \"private_bytes\": " << memory.PrivateUsage
                << ",\n  \"peak_working_set_bytes\": " << memory.PeakWorkingSetSize
                << ",\n  \"peak_commit_bytes\": " << memory.PeakPagefileUsage << "\n}\n";
            report.close();
            if (!report) throw std::runtime_error("Cannot write loading benchmark output");
            return EXIT_SUCCESS;
        }
        return app.Run();
    }
    catch (const ConsoleConfigError&)
    {
        return EXIT_FAILURE;
    }
    catch (const DxException& error)
    {
        Log::Error("Application", "DirectX failure: ", error.ToString());
        Log::Flush();
        MessageBoxW(nullptr, error.ToString().c_str(), L"Bistro App", MB_OK | MB_ICONERROR);
    }
    catch (const std::exception& error)
    {
        Log::Error("Application", "Application failed: ", error.what());
        Log::Flush();
        MessageBoxA(nullptr, error.what(), "Bistro App", MB_OK | MB_ICONERROR);
    }
    return EXIT_FAILURE;
}
