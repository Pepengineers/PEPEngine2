#include <App.Base/App.h>
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
#include "App.Base/Systems/FlyingPropSystem.h"
#include "App.Base/LoadingSettings.h"

namespace
{
    class BistroApp final : public App
    {
    public:
        explicit BistroApp(const HINSTANCE instance) : App(instance) {}

        void BenchmarkFrame()
        {
            MSG message;
            while (PeekMessage(&message, nullptr, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&message);
                DispatchMessage(&message);
            }
            Timer.Reset();
            Timer.Tick();
            Update(Timer);
            Render(Timer);
            Locator.GetModule<RenderModule>()->WaitForUploads();
        }

        bool Initialize() override
        {
            if (!App::Initialize()) return false;
            auto& ecs = Locator.GetModule<SceneManagerModule>()->GetWorld()->GetECS();
            // Moving benchmark casters require fresh maps even when the fixtures are stationary.
            ecs.ForEach<LightComponent>([](Entity, LightComponent& light) { light.ShadowUpdate = ShadowUpdateMode::EveryFrame; });
            return true;
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
            Locator.RegisterModule(std::make_shared<SceneManagerModule>(scenePath));
            return BenchmarkEngine::AddModules();
        }

        void Update(const GameTimer& timer) override
        {
            OnKeyboardInput(timer);
            Locator.GetModule<RenderModule>()->Update();
            auto& world = *Locator.GetModule<SceneManagerModule>()->GetWorld();
            if (world.IsPaused()) return;
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
                work = _gpuData.Schedule(_jobs, world, dt, work);
                work = _reflectionProbes.Schedule(_jobs, world, dt, work);
                work.Complete();
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
        std::wstring budgetArgument;
        int argumentCount = 0;
        auto arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
        if (!arguments) throw std::runtime_error("Cannot read startup arguments");
        for (int index = 1; index < argumentCount; ++index)
        {
            const std::wstring_view argument(arguments[index]);
            constexpr std::wstring_view budgetPrefix = L"-Loading.TransientBudgetMiB=";
            if (argument.substr(0, budgetPrefix.size()) == budgetPrefix) budgetArgument = argument;
            constexpr std::wstring_view prefix = L"-Loading.BenchmarkOutput=";
            if (argument.substr(0, prefix.size()) == prefix)
                benchmarkOutput = std::wstring(argument.substr(prefix.size()));
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
            const auto ready = std::chrono::steady_clock::now();
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
