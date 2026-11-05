#include "App.Base/App.h"
#include "Rendering/BistroRenderPipeline.h"
#include "../src/BistroScene.h"
#include "Engine.Core/ConsoleModule.h"
#include <d3d12sdklayers.h>
#include <iostream>
#include <stdexcept>

static void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

class BistroSSRApp final : public App
{
public:
    BistroSSRApp() : App(GetModuleHandleW(nullptr)) {}
    bool AddModules() override
    {
        auto scenes = std::make_shared<SceneManagerModule>(
            std::filesystem::path(ASSETS_FOLDER) / "Scenes" / "Amazon Lumberyard Bistro");
        Locator.RegisterModule(std::make_shared<RenderModule>(GetWindow(), &Timer,
            std::make_unique<BistroRenderPipeline>(scenes)));
        ConfigureBistroProps(*scenes);
        Locator.RegisterModule(scenes);
        return BenchmarkEngine::AddModules();
    }

    void Frame()
    {
        MSG message;
        while (PeekMessage(&message, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&message);
            DispatchMessage(&message);
        }
        Timer.Tick();
        auto render = Locator.GetModule<RenderModule>();
        render->Update();
        auto& world = *Locator.GetModule<SceneManagerModule>()->GetWorld();
        UpdatePhysics(world, 1.0f / 60);
        auto work = _lights.Schedule(_jobs, world, 1.0f / 60);
        work = _gpuData.Schedule(_jobs, world, 1.0f / 60, work);
        work.Complete();
        Render(Timer);
        render->WaitForUploads();
    }

    void ViewExistingBottles()
    {
        auto& world = *Locator.GetModule<SceneManagerModule>()->GetWorld();
        auto& ecs = world.GetECS();
        bool found = false;
        Vector3 target;
        UINT bottles = 0;
        ecs.ForEach<NameComponent, StaticMeshRenderComponent>([&](Entity, const NameComponent& name, const StaticMeshRenderComponent& renderer)
        {
            if (name.value.find("Bottle") != std::string::npos || name.value.find("bottle") != std::string::npos)
            {
                ++bottles;
                if (!found) { target = Vector3(renderer.Bounds.Center); found = true; }
            }
        });
        Require(found, "Existing Bistro bottles were not found");
        auto& camera = ecs.Get<TransformComponent>(world.ActiveCamera);
        camera.Location = target + Vector3(0, 20, -180);
        camera.Rotation = Vector3(6, 0, 0);
        camera.DirtyFlag = true;
        ecs.Get<CameraComponent>(world.ActiveCamera).InvalidateHistory();
        std::cout << "Existing bottles=" << bottles << " camera=" << camera.Location.x << ',' << camera.Location.y << ',' << camera.Location.z << '\n';
        ecs.ForEach<LightComponent>([](Entity, LightComponent& light) { light.ShadowUpdate = ShadowUpdateMode::EveryFrame; });
    }

    RenderModule& Renderer() { return *Locator.GetModule<RenderModule>(); }
};

int main(int argc, char**)
{
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\PEPEngine2FeatureGpuValidation");
    if (!mutex) return 1;
    DWORD wait = WaitForSingleObject(mutex, INFINITE);
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) { CloseHandle(mutex); return 1; }
    int result = 1;
    try
    {
        ComPtr<ID3D12Debug> debug;
        ThrowIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
        debug->EnableDebugLayer();
        BistroSSRApp app;
        Require(app.Initialize(), "Bistro initialization failed");
        ComPtr<ID3D12Device> device;
        ThrowIfFailed(app.Renderer().GetCurrentPrimaryFrameConstants()->CameraCB->GetResource().D3DResource->GetDevice(IID_PPV_ARGS(&device)));
        ComPtr<ID3D12InfoQueue> info;
        ThrowIfFailed(device.As(&info));
        info->ClearStoredMessages();
        app.Frame();
        app.ViewExistingBottles();
        for (int quality : { 0, 1, 2, 3, 0, 3 })
        {
            Require(ConsoleModule().FindConsoleVariable(L"Render.SSR.Quality").SetFromString(std::to_string(quality)), "Quality change rejected");
            for (int i = 0; i < 4; ++i) app.Frame();
        }
        for (auto size : { std::pair{ 1111, 777 }, { 1280, 800 } })
        {
            app.GetWindow()->SetWindowSize(static_cast<uint16_t>(size.first), static_cast<uint16_t>(size.second));
            app.Renderer().OnResize();
            for (int i = 0; i < 4; ++i) app.Frame();
        }
        bool failed = false;
        UINT displayed = 0;
        for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i)
        {
            SIZE_T size = 0;
            info->GetMessage(i, nullptr, &size);
            std::vector<BYTE> bytes(size);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
            info->GetMessage(i, message, &size);
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_WARNING)
            {
                if (displayed++ < 10) std::cerr << message->pDescription << '\n';
                failed = true;
            }
        }
        Require(!failed, "Bistro HDR/SSSR/WBOIT/XeSS/output debug-layer validation failed");
        std::cout << "Bistro existing bottles + moving props, quality/Off/re-enable, odd resize, HDR XeSS/output passed" << std::endl;
        if (argc > 1)
        {
            std::cout << "Visual inspection ready; press Enter to exit" << std::endl;
            std::cin.get();
        }
        result = 0;
    }
    catch (const DxException& error) { std::wcerr << error.ToString() << '\n'; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
    ReleaseMutex(mutex); CloseHandle(mutex);
    return result;
}
