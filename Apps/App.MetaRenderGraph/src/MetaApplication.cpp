#include "MetaApplication.h"
#include "App.Base/App.h"
#include "Common/RuntimePaths.h"
#include "Engine.Core/ConsoleModule.h"
#include "Engine.Core/Logging/Logger.h"
#include <d3d12sdklayers.h>
#include <fstream>
#include <stdexcept>

namespace
{
    void Require(bool value, const char* message)
    {
        if (!value) throw std::runtime_error(message);
    }

    // Runs before BenchmarkEngine's constructor reads and locks startup CVars.
    // Both files belong exclusively to this executable's output directory.
    void PrepareStartup()
    {
        const auto root = Engine::Core::GetExecutableDirectory();
        std::filesystem::create_directories(root / "Config");
        std::filesystem::copy_file(root / "Configs/Startup.ini", root / "Config/Engine.ini",
            std::filesystem::copy_options::overwrite_existing);
        // Initialize the registry now so the invariant is checked before any device is opened.
        ConsoleModule console;
        console.Initialize();
        Require(console.FindConsoleVariable(L"Render.PrimaryAdapter").GetInt() == 0, "Unexpected primary adapter");
        Require(console.FindConsoleVariable(L"Render.Particles.SecondaryAdapter").GetInt() == -1, "Particle offload must be disabled");
        Require(!console.FindConsoleVariable(L"Render.Particles.AsyncCompute").GetBool(), "Particle async must be disabled");
        Require(!console.FindConsoleVariable(L"Render.AO.AsyncCompute").GetBool(), "AO async must be disabled");
        IConsoleVariable* water = nullptr;
        Require(!console.TryFindConsoleVariable(L"Render.Fluid.SecondaryAdapter", water), "U1 must not link Bistro water runtime");
        Log::Info("MetaRenderGraph", "REFERENCE ONLY; primary=0; particle secondary=-1; water runtime absent; single DIRECT execution");
    }

    class MetaApplication final : public App
    {
    public:
        MetaApplication() : App(GetModuleHandleW(nullptr)) {}
        bool Initialize() override
        {
            if (!App::Initialize()) return false;
            auto scenes = Locator.GetModule<SceneManagerModule>();
            Require(scenes->LoadWorld(Engine::Core::GetExecutableDirectory() / "Configs/SmokeWorld.yaml"), "Cannot create smoke world");
            auto& world = *scenes->GetWorld();
            auto emitter = world.GetECS().CreateEntity();
            emitter.AddComponent<TransformComponent>();
            GDX12ParticleEmitterSettings particles;
            particles.Seed = 13;
            particles.MaxParticles = 256;
            particles.SharedSimulation = true; // Eligible emitter still stays on the primary GPU.
            particles.Blend = ParticleBlend::Additive;
            particles.EmissionRate = 0;
            auto& component = emitter.AddComponent<ParticleEmitterComponent>();
            component.SetSettings(particles);
            component.Emit(64);
            SetWindowTextW(GetWindow()->GetWindowHandle(), L"MetaRenderGraph U1 - procedural reference only");
            return true;
        }
        void Smoke()
        {
            auto renderer = Locator.GetModule<RenderModule>();
            ComPtr<ID3D12InfoQueue> info;
            ThrowIfFailed(renderer->GetPrimaryDevice()->GetDevice().As(&info));
            Timer.Reset();
            Timer.Stop(); // Fixed zero dt: reproducible initial burst, no wall-clock simulation.
            const auto frame = [&]
            {
                MSG message{};
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
                { TranslateMessage(&message); DispatchMessageW(&message); }
                Update(Timer);
                Render(Timer);
            };
            for (int i = 0; i < 4; ++i) frame();
            const auto before = renderer->GetFrameSubmissionId();
            Require(SetWindowPos(GetWindow()->GetWindowHandle(), nullptr, 0, 0, 800, 600,
                SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE, "Window resize failed");
            for (int i = 0; i < 4; ++i) frame();
            uint16_t width, height;
            GetWindow()->GetWindowSize(width, height);
            Require(width != 1280 && width > 0 && height > 0, "Resize did not change client extent");
            Require(renderer->GetFrameSubmissionId() == before + 4, "Frames did not submit after resize");
            renderer->DrainFrameWork();
            for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i)
            {
                SIZE_T size = 0;
                ThrowIfFailed(info->GetMessage(i, nullptr, &size));
                std::vector<BYTE> bytes(size);
                auto* message = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
                ThrowIfFailed(info->GetMessage(i, message, &size));
                if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) throw std::runtime_error(message->pDescription);
            }
            const auto root = Engine::Core::GetExecutableDirectory() / "Logs";
            std::ofstream graph(root / "reference.dot");
            renderer->WriteRenderGraph(graph);
            Require(bool(graph), "Cannot write reference graph");
            Log::Info("MetaRenderGraph", "Smoke passed: 8 presented frames, actual window resize to ", width, "x", height,
                "; D3D12 errors=0; seed=13; dt=0; burst=64");
            // Exercise the normal window close / WM_DESTROY / WM_QUIT loop as well.
            PostMessageW(GetWindow()->GetWindowHandle(), WM_CLOSE, 0, 0);
            Require(Run() == 0, "Window shutdown failed");
        }
    };
}

int RunMetaApplication(bool smoke)
{
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    try
    {
        PrepareStartup();
        if (smoke)
        {
            ComPtr<ID3D12Debug> debug;
            ThrowIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
            debug->EnableDebugLayer();
        }
        {
            MetaApplication app;
            Require(app.Initialize(), "Application initialization failed");
            if (smoke) app.Smoke();
            else return app.Run();
        }
        Log::Info("MetaRenderGraph", "Smoke shutdown complete");
        Log::Flush();
        return 0;
    }
    catch (const DxException& error) { Log::Error("MetaRenderGraph", error.ToString()); }
    catch (const std::exception& error) { Log::Error("MetaRenderGraph", error.what()); }
    Log::Flush();
    return 1;
}
