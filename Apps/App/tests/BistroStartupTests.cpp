#include "App.Base/App.h"
// Exercise the real bootstrap: the other scene tests register their own modules.
#define WinMain BistroEntryPointForTest
#include "../src/App.cpp"
#undef WinMain
#include "Engine.RendererDX12/GDX12FrameGeneration.h"
#include <iostream>
#include <thread>

int main()
{
    try
    {
        BistroApp app(GetModuleHandleW(nullptr));
        if (!app.Initialize()) throw std::runtime_error("Bistro startup failed");
        ShowWindow(app.GetWindow()->GetWindowHandle(), SW_HIDE);
        auto& world = *BenchmarkEngine::GetLocator().GetModule<SceneManagerModule>()->GetWorld();
        auto& ecs = world.GetECS();
        bool passed = true;
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
        if (passed) std::cout << "Background rendering: defaults, runtime CVar, focus, minimize and resize passed.\n";
        if (passed) std::cout << "Bistro startup: both scene parts, 346 props and stable entrance camera passed.\n";
        return passed ? 0 : 1;
    }
    catch (const DxException& error) { std::wcerr << error.ToString() << '\n'; return 1; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
