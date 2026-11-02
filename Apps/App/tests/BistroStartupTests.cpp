#include "App.Base/App.h"
// Exercise the real bootstrap: the other scene tests register their own modules.
#define WinMain BistroEntryPointForTest
#include "../src/App.cpp"
#undef WinMain
#include "Engine.RendererDX12/GDX12FrameGeneration.h"
#include <iostream>
#include <thread>
#include <d3d12sdklayers.h>

int main()
{
    try
    {
        ComPtr<ID3D12Debug> debug;
        ThrowIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
        debug->EnableDebugLayer();
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
                ThrowIfFailed(device.As(&info));
            }
        });
        check(particleBudget == 250000, "Default Bistro budget must be 250000 particles");
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
        return passed ? 0 : 1;
    }
    catch (const DxException& error) { std::wcerr << error.ToString() << '\n'; return 1; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
