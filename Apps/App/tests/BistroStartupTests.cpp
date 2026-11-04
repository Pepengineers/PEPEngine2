#include <cstdint>
#include "App.Base/App.h"
// Exercise the real bootstrap: the other scene tests register their own modules.
#define WinMain BistroEntryPointForTest
#include "../src/App.cpp"
#undef WinMain
#include "Engine.RendererDX12/GDX12FrameGeneration.h"
#include "Engine.UI/UI_test.h"
#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>
#include <iostream>
#include <cstring>
#include <thread>
#include <d3d12sdklayers.h>

static void CheckEnumCVars()
{
    const auto require = [](bool condition, const char* message)
    { if (!condition) throw std::runtime_error(message); };
    for (const auto name : { L"Render.AO.Quality", L"Render.SSR.Quality", L"Render.DOF.Quality",
        L"Render.MotionBlur.Quality", L"Render.Reflection.Quality", L"Render.Grass.Quality",
        L"Render.Wind.Quality", L"Render.Particles.Quality", L"Render.Fluid.Quality",
        L"Render.Skinning.Quality", L"Render.Clouds.Quality", L"Render.SkyBox.Quality", L"Render.SkyAtmosphere.Quality" })
    {
        const auto& quality = ConsoleModule{}.FindConsoleVariable(name);
        require(quality.GetType() == EConsoleVariableType::Enumeration && quality.GetEnumCount() == 4 &&
            quality.GetEnumFirstValue() == 0, "Every effect quality must use an enum dropdown");
    }
    for (const auto name : { L"Render.Upscaler.Provider", L"Render.Upscaler.Quality",
        L"Render.FrameGeneration.Provider", L"Render.AO.Implementation", L"Render.AO.ExecutionPoint",
        L"Render.AO.CACAO.Quality", L"App.Grass.Implementation", L"App.Grass.Wind.Algorithm", L"App.Particles.Simulation" })
        require(ConsoleModule{}.FindConsoleVariable(name).GetType() == EConsoleVariableType::Enumeration,
            "Every provider and mode selector must use an enum dropdown");
    for (const auto* variable : ConsoleModule{}.GetConsoleVariables())
        for (int i = 0; i < variable->GetEnumCount(); ++i)
            require(!variable->GetEnumValueName(variable->GetEnumFirstValue() + i).empty(),
                "Every enum dropdown entry must have a name");
    require(ConsoleModule{}.FindConsoleVariable(L"Render.AO.Quality").GetEnumValueName(2) == "Medium" &&
        ConsoleModule{}.FindConsoleVariable(L"Render.AO.ExecutionPoint").GetEnumValueName(1) == "Before lighting" &&
        ConsoleModule{}.FindConsoleVariable(L"Render.Upscaler.Provider").GetEnumValueName(1) == "XeSS" &&
        ConsoleModule{}.FindConsoleVariable(L"Render.Upscaler.Quality").GetEnumValueName(0) == "SDK default" &&
        ConsoleModule{}.FindConsoleVariable(L"Render.AO.CACAO.Quality").GetEnumValueName(-1) == "Inherit AO quality",
        "Dropdown labels must use enum identifiers or declared display names");
    std::cout << "Effect quality/provider/mode CVar metadata passed\n";
}

int main(int argc, char** argv)
{
    try
    {
        CheckEnumCVars();
        if (argc > 1 && std::strcmp(argv[1], "--cvars-only") == 0) return 0;
        const bool checkDebugLayer = argc > 1 && std::strcmp(argv[1], "--check-debug-layer") == 0;
        if (!checkDebugLayer)
        {
            ComPtr<ID3D12Debug> debug;
            ThrowIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
            debug->EnableDebugLayer();
        }
        BistroReflections::ShowBounds.SetFromString("false");
        BistroBounds::Show.SetFromString("false");
        BistroApp app(GetModuleHandleW(nullptr));
        if (!app.Initialize()) throw std::runtime_error("Bistro startup failed");
        ShowWindow(app.GetWindow()->GetWindowHandle(), SW_HIDE);
        auto& world = *BenchmarkEngine::GetLocator().GetModule<SceneManagerModule>()->GetWorld();
        auto& ecs = world.GetECS();
        bool passed = true;
        ComPtr<ID3D12InfoQueue> info;
        const auto check = [&](bool condition, const char* message)
        {
            if (!condition) { std::cerr << message << '\n'; passed = false; }
        };
        const auto expectedScene = std::filesystem::path(ASSETS_FOLDER) / "Scenes/Amazon Lumberyard Bistro";
        std::cout << "Loaded scene: " << world.GetPath() << '\n';
        check(world.GetPath() == expectedScene, "App must load the complete Bistro directory");
        bool exterior = false, interior = false;
        ecs.ForEach<NameComponent, StaticMeshRenderComponent>([&](Entity, const NameComponent& name, const StaticMeshRenderComponent&)
        {
            exterior |= name.value == "Exterior_exterior";
            interior |= name.value == "Interior_interior";
        });
        check(exterior && interior, "Bistro Exterior and Interior entities must both be present");
        check(ecs.GetPool<FlyingPropComponent>().Size() == 346, "Bistro must retain its 346 original props");
        check(ecs.GetPool<ParticleEmitterComponent>().Size() == 8, "Bistro must have eight effect emitters");
        UINT particleBudget = 0;
        ecs.ForEach<ParticleEmitterComponent, TransformComponent>([&](Entity entity, const ParticleEmitterComponent& emitter, const TransformComponent& transform)
        {
            const auto& settings = emitter.GetSettings();
            if (emitter.IsEnabled()) particleBudget += settings.MaxParticles;
            if (ecs.Get<NameComponent>(entity).value == "BistroEntranceFountain")
            {
                check(transform.Location == Vector3(-170, 45, -170), "Fountain must be outside the main Bistro entrance");
                check(settings.Texture && settings.AtlasColumns == 8 && settings.AtlasRows == 8 &&
                    settings.Texture->PrimaryDeviceTexture && settings.DepthCollision && emitter.GetBurstTotal() > 0 &&
                    settings.Blend == ParticleBlend::Additive, "Fountain must use the MGPU atlas and depth collision");
            }
            if (settings.Texture && settings.Texture->PrimaryDeviceTexture)
            {
                ComPtr<ID3D12Device> device;
                ThrowIfFailed(settings.Texture->PrimaryDeviceTexture->GetResource()->D3DResource->GetDevice(IID_PPV_ARGS(&device)));
                const HRESULT debugResult = device.As(&info);
                if (!checkDebugLayer || debugResult != E_NOINTERFACE) ThrowIfFailed(debugResult);
            }
        });
        check(particleBudget == 250000, "Default Bistro budget must be 250000 particles");
        if (checkDebugLayer)
        {
            const bool expected = argc > 2 && std::strcmp(argv[2], "-d3ddebug") == 0;
            check(static_cast<bool>(info) == expected, "D3D12 debug layer must require an explicit -d3ddebug argument");
            if (passed) std::cout << "D3D12 debug layer opt-in passed: enabled=" << expected << '\n';
            return passed ? 0 : 1;
        }
        check(!ecs.Has<SplineFollowComponent>(world.ActiveCamera) && !ecs.Has<LookAtTargetComponent>(world.ActiveCamera),
            "The free camera must not inherit benchmark spline/look-at controls");
        SplineFollowSystem spline;
        LookAtTargetSystem lookAt;
        spline.Tick(world, 1.0f / 60);
        lookAt.Tick(world, 1.0f / 60);
        const auto& camera = ecs.Get<TransformComponent>(world.ActiveCamera);
        check(camera.Location == Vector3(-300, 200, 20) && camera.Rotation == Vector3(0, 117, 0),
            "Startup camera must stay at the Bistro entrance after system updates");

        auto& background = ConsoleModule{}.FindConsoleVariable(L"App.RenderInBackground");
        check(background.GetBool(), "Background rendering must be enabled by default");
        auto render = BenchmarkEngine::GetLocator().GetModule<RenderModule>();
        const auto window = app.GetWindow()->GetWindowHandle();
        app.MsgProc(window, WM_ACTIVATE, WA_INACTIVE, 0);
        check(!render->GetFrameGenerationStatus().Suspended, "Alt+Tab must keep rendering active by default");
        app.BenchmarkFrame();
        app.BenchmarkFrame();
        check(ImGui::GetCurrentContext() && ImGui::GetDrawData() && ImGui::GetDrawData()->TotalVtxCount > 0,
            "Image quality window must generate visible ImGui geometry");
        {
            enum class ProbeMode : std::uint8_t { First, Second, Third, Count };
            AutoConsoleVariable<ProbeMode> discovered(L"Render.AutoDiscoveryTest.NewSetting", ProbeMode::Second,
                L"Runtime registration probe: 0 First, 1 Second, 2 Third");
            AutoConsoleVariable<float> size(L"Render.AutoDiscoveryTest.Size", 1.25f);
            AutoConsoleVariable<int> count(L"Render.AutoDiscoveryTest.Count", 4);
            AutoConsoleVariable<bool> enabled(L"Render.AutoDiscoveryTest.Enabled", true);
            const auto logPath = ConsoleModule::GetDefaultConfigPath().parent_path().parent_path() / "Logs/GraphicsSettingsUITest.log";
            auto logFilename = logPath.string();
            ImGuiContextHook hook;
            hook.Type = ImGuiContextHookType_NewFramePost;
            hook.UserData = &logFilename;
            hook.Callback = [](ImGuiContext*, ImGuiContextHook* current)
            { ImGui::LogToFile(0, static_cast<const std::string*>(current->UserData)->c_str()); };
            const auto captureSettings = [&]
            {
                const auto previousBytes = std::filesystem::exists(logPath) ? std::filesystem::file_size(logPath) : 0;
                const auto hookId = ImGui::AddContextHook(ImGui::GetCurrentContext(), &hook);
                app.BenchmarkFrame();
                ImGui::RemoveContextHook(ImGui::GetCurrentContext(), hookId);
                ImGui::LogFinish();
                std::ifstream log(logPath);
                log.seekg(static_cast<std::streamoff>(previousBytes));
                return std::string((std::istreambuf_iterator<char>(log)), std::istreambuf_iterator<char>());
            };
            const auto text = captureSettings();
            const auto group = text.find("AutoDiscoveryTest");
            check(group != std::string::npos && text.find("AutoDiscoveryTest", group + 1) == std::string::npos &&
                text.find("NewSetting") != std::string::npos && text.find("Size") != std::string::npos &&
                text.find("Count") != std::string::npos && text.find("Enabled") != std::string::npos &&
                text.find("Second") != std::string::npos &&
                discovered.GetType() == EConsoleVariableType::Enumeration && discovered.GetEnumCount() == 3,
                "New typed CVars must appear in one shared name-prefix group without UI changes");
            auto* settings = ImGui::FindWindowByName("Settings (F1)");
            const auto treeId = ImHashStr("SettingsTree", 0, settings->ID);
            const auto appId = ImHashStr("App", 0, treeId);
            const auto boundsId = ImHashStr("Bounds", 0, appId);
            settings->StateStorage.SetInt(appId, 0);
            const auto closedApp = captureSettings();
            check(closedApp.find("BistroTableSteam") == std::string::npos && closedApp.find("NewSetting") != std::string::npos,
                "Closing App must hide all its descendants without hiding the Render tree");
            settings->StateStorage.SetInt(appId, 1);
            settings->StateStorage.SetInt(boundsId, 0);
            check(captureSettings().find("BistroTableSteam") == std::string::npos,
                "Closing Bounds must hide every individual bound");
            settings->StateStorage.SetInt(appId, 0);
            app.BenchmarkFrame();
            settings->StateStorage.SetInt(appId, 1);
            check(captureSettings().find("BistroTableSteam") == std::string::npos,
                "Reopening App must preserve the collapsed Bounds folder");
            settings->StateStorage.SetInt(boundsId, 1);
            check(captureSettings().find("BistroTableSteam") != std::string::npos,
                "Reopening Bounds must restore its children");
        }
        ImGui::GetIO().WantCaptureKeyboard = true;
        check(Engine::UI::WantsKeyboardInput() && Engine::UI::HandleMessage(window, WM_CHAR, 'W', 0),
            "Editing settings must capture keyboard input before camera handling");
        ImGui::GetIO().WantCaptureMouse = true;
        check(Engine::UI::HandleMessage(window, WM_MOUSEWHEEL, 0, 0), "Settings must capture the mouse wheel");
        app.MsgProc(window, WM_KEYUP, VK_F1, 0);
        app.BenchmarkFrame();
        check(ImGui::GetDrawData()->TotalVtxCount == 0 && !Engine::UI::WantsKeyboardInput(),
            "F1 must hide settings and release camera input");
        app.MsgProc(window, WM_KEYUP, VK_F1, 0);
        app.BenchmarkFrame();
        check(ImGui::GetDrawData()->TotalVtxCount > 0, "F1 must reopen settings");
        auto& frameGeneration = ConsoleModule{}.FindConsoleVariable(L"Render.FrameGeneration.Provider");
        check(frameGeneration.SetFromString("3"), "FG selection must be writable on the real Bistro path");
        app.BenchmarkFrame();
        check(render->GetFrameGenerationStatus().Requested == GDX12FrameGenerationProvider::Intel,
            "Bistro Update must begin the frame and apply a runtime FG selection");
        check(frameGeneration.SetFromString("0"), "FG Off must remain writable");
        app.BenchmarkFrame();
        check(render->GetFrameGenerationStatus().Requested == GDX12FrameGenerationProvider::Off,
            "Bistro Update must apply FG Off on the next frame");
        for (unsigned frame = 0; frame < 90; ++frame) app.BenchmarkFrame();
        auto& scale = ConsoleModule{}.FindConsoleVariable(L"App.Particles.CountScale");
        for (const char* value : {"4", "0", "-2", "0.5", "1"})
        {
            check(scale.SetFromString(value), "Particle scale must change at runtime");
            app.BenchmarkFrame();
            UINT total = 0;
            ecs.ForEach<ParticleEmitterComponent>([&](Entity, const ParticleEmitterComponent& emitter)
            { if (emitter.IsEnabled()) total += emitter.GetSettings().MaxParticles; });
            check(total == BistroParticles::GetBudget(), "Runtime particle scale was not applied to every emitter");
        }
        check(camera.Location == Vector3(-300, 200, 20), "Background rendering must not move the free camera");
        check(background.SetFromString("0"), "Background rendering CVar must be writable at runtime");
        app.MsgProc(window, WM_ACTIVATE, WA_INACTIVE, 0);
        check(render->GetFrameGenerationStatus().Suspended, "Disabling background rendering must pause inactive windows");
        app.MsgProc(window, WM_ACTIVATE, WA_ACTIVE, 0);
        check(!render->GetFrameGenerationStatus().Suspended, "Activating the window must resume rendering");
        check(background.SetFromString("1"), "Background rendering must be re-enableable");
        app.MsgProc(window, WM_ACTIVATE, WA_INACTIVE, 0);
        app.MsgProc(window, WM_SIZE, SIZE_MINIMIZED, 0);
        check(render->GetFrameGenerationStatus().Suspended, "Minimizing must still pause rendering");
        app.MsgProc(window, WM_ACTIVATE, WA_ACTIVE, 0);
        check(render->GetFrameGenerationStatus().Suspended, "Activation must not resume a minimized window");
        RECT client{};
        GetClientRect(window, &client);
        app.MsgProc(window, WM_SIZE, SIZE_RESTORED, MAKELPARAM(client.right, client.bottom));
        check(!render->GetFrameGenerationStatus().Suspended, "Restoring must resume rendering");
        app.MsgProc(window, WM_ENTERSIZEMOVE, 0, 0);
        app.MsgProc(window, WM_ACTIVATE, WA_ACTIVE, 0);
        check(render->GetFrameGenerationStatus().Suspended, "Activation must not resume a window during resize");
        app.MsgProc(window, WM_EXITSIZEMOVE, 0, 0);
        check(!render->GetFrameGenerationStatus().Suspended, "Finishing resize must resume rendering");

        background.SetFromString("0");
        app.MsgProc(window, WM_ACTIVATE, WA_INACTIVE, 0);
        const auto windowThread = GetCurrentThreadId();
        std::thread resume([&]
        {
            Sleep(100);
            background.SetFromString("1");
            Sleep(250);
            PostThreadMessageW(windowThread, WM_QUIT, 0, 0);
        });
        try { app.Run(); }
        catch (...) { resume.join(); throw; }
        resume.join();
        check(!render->GetFrameGenerationStatus().Suspended,
            "Changing the CVar while paused must resume the main loop without another activation event");
        if (info)
            for (UINT64 index = 0; index < info->GetNumStoredMessages(); ++index)
            {
                SIZE_T bytes = 0; info->GetMessage(index, nullptr, &bytes);
                std::vector<BYTE> storage(bytes);
                auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
                ThrowIfFailed(info->GetMessage(index, message, &bytes));
                check(message->Severity > D3D12_MESSAGE_SEVERITY_ERROR, message->pDescription);
            }
        if (passed) std::cout << "Bistro effects: eight emitters, mesh fireflies, 1x/4x/fraction/<=0 scales and 90 frames with no debug-layer errors passed.\n";
        if (passed) std::cout << "Background rendering: defaults, runtime CVar, focus, minimize and resize passed.\n";
        if (passed) std::cout << "Bistro startup: both scene parts, 346 props and stable entrance camera passed.\n";
        if (passed) std::cout << "Image quality UI: automatic enum discovery, draw data, F1 and keyboard/mouse capture passed.\n";
        return passed ? 0 : 1;
    }
    catch (const DxException& error) { std::wcerr << error.ToString() << '\n'; return 1; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
