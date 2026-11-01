#include "App.Base/App.h"
// Exercise the real bootstrap: the other scene tests register their own modules.
#define WinMain BistroEntryPointForTest
#include "../src/App.cpp"
#undef WinMain
#include <iostream>

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
        if (passed) std::cout << "Bistro startup: both scene parts, 346 props and stable entrance camera passed.\n";
        return passed ? 0 : 1;
    }
    catch (const DxException& error) { std::wcerr << error.ToString() << '\n'; return 1; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
