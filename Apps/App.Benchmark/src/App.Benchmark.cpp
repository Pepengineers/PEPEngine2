// App.Benchmark.cpp

#include <Engine.Core/BenchmarkEngine.h>
#include <App.Base/App.h>
#include <App.Base/Modules/RenderModule.h>

#include <Engine.Core/Registries/MeshRegistry.h>
#include <Engine.Core/Registries/TextureRegistry.h>
#include <Engine.Core/AssetManager.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cwchar>
#include <exception>

namespace
{
    void ConfigureSingleGPUXeSS(FrameGraph& graph, const FrameGraphInputs& inputs)
    {
        graph.SetTransportQueuePolicy(TransportQueuePolicy::Auto);
        auto culling = graph.AddPrimaryGPUPass<GDX12GPUCullingPass>("Frustum Culling");
        auto opaque = graph.AddPrimaryGPUPass<GDX12OpaquePass>("Opaque");
        auto transparency = graph.AddPrimaryGPUPass<GDX12WBOITTransparencyPass>("Transparency");
        auto composition = graph.AddPrimaryGPUPass<GDX12WBOITCompositionPass>("Composition");
        auto clear = graph.AddPrimaryGPUPass<GDX12TextureClearPass>("Back Buffer Clear");
        auto upscale = graph.AddPrimaryGPUPass<GDX12XeSSUpscalePass>("XeSS");
        auto output = graph.AddPrimaryGPUPass<GDX12OutputToScreenPass>("Output To Screen");

        graph.ConnectNodes(inputs.BackBuffer, clear.GetInput("InTexture"));
        graph.ConnectNodes(inputs.DepthStencil, clear.GetInput("InDepthBuffer"));
        graph.ConnectNodes(clear.GetOutput("OutClearedTexture"), output.GetInput("InBackBuffer"));
        graph.ConnectNodes(culling.GetOutput("OutOpaqueDrawStream"), opaque.GetInput("InDrawStream"));
        graph.ConnectNodes(culling.GetOutput("OutTransparentDrawStream"), transparency.GetInput("InDrawStream"));
        graph.ConnectNodes(opaque.GetOutput("OutDepthBuffer"), transparency.GetInput("InDepthBuffer"));
        graph.ConnectNodes(opaque.GetOutput("OutColorTexture"), composition.GetInput("InOpaqueTexture"));
        graph.ConnectNodes(transparency.GetOutput("OutAccumulationTexture"), composition.GetInput("InAccumulationTexture"));
        graph.ConnectNodes(transparency.GetOutput("OutRevealageTexture"), composition.GetInput("InRevealageTexture"));
        graph.ConnectNodes(composition.GetOutput("OutColorTexture"), upscale.GetInput("InColorTexture"));
        graph.ConnectNodes(opaque.GetOutput("OutDepthBuffer"), upscale.GetInput("InDepthBuffer"));
        graph.ConnectNodes(opaque.GetOutput("OutVelocityBuffer"), upscale.GetInput("InVelocityBuffer"));
        graph.ConnectNodes(upscale.GetOutput("OutColorTexture"), output.GetInput("InColorTexture"));
    }

    void ConfigureDualGPUXeSS(FrameGraph& graph, const FrameGraphInputs& inputs)
    {
        graph.SetTransportQueuePolicy(TransportQueuePolicy::Auto);
        auto culling = graph.AddPrimaryGPUPass<GDX12GPUCullingPass>("Frustum Culling");
        auto opaque = graph.AddPrimaryGPUPass<GDX12OpaquePass>("Opaque");
        auto transparency = graph.AddPrimaryGPUPass<GDX12WBOITTransparencyPass>("Transparency");
        auto composition = graph.AddPrimaryGPUPass<GDX12WBOITCompositionPass>("Composition");
        auto clear = graph.AddSecondaryGPUPass<GDX12TextureClearPass>("Back Buffer Clear");
        auto upscale = graph.AddSecondaryGPUPass<GDX12XeSSUpscalePass>("XeSS");
        auto output = graph.AddSecondaryGPUPass<GDX12OutputToScreenPass>("Output To Screen");

        graph.ConnectNodes(inputs.BackBuffer, clear.GetInput("InTexture"));
        graph.ConnectNodes(inputs.DepthStencil, clear.GetInput("InDepthBuffer"));
        graph.ConnectNodes(clear.GetOutput("OutClearedTexture"), output.GetInput("InBackBuffer"));
        graph.ConnectNodes(culling.GetOutput("OutOpaqueDrawStream"), opaque.GetInput("InDrawStream"));
        graph.ConnectNodes(culling.GetOutput("OutTransparentDrawStream"), transparency.GetInput("InDrawStream"));
        graph.ConnectNodes(opaque.GetOutput("OutDepthBuffer"), transparency.GetInput("InDepthBuffer"));
        graph.ConnectNodes(opaque.GetOutput("OutColorTexture"), composition.GetInput("InOpaqueTexture"));
        graph.ConnectNodes(transparency.GetOutput("OutAccumulationTexture"), composition.GetInput("InAccumulationTexture"));
        graph.ConnectNodes(transparency.GetOutput("OutRevealageTexture"), composition.GetInput("InRevealageTexture"));
        graph.ConnectNodes(composition.GetOutput("OutColorTexture"), upscale.GetInput("InColorTexture"));
        graph.ConnectNodes(opaque.GetOutput("OutDepthBuffer"), upscale.GetInput("InDepthBuffer"));
        graph.ConnectNodes(opaque.GetOutput("OutVelocityBuffer"), upscale.GetInput("InVelocityBuffer"));
        graph.ConnectNodes(upscale.GetOutput("OutColorTexture"), output.GetInput("InColorTexture"));
    }

    struct BenchmarkStage
    {
        const char* Name;
        RenderModule::PipelineConfiguration Configuration;
        std::chrono::seconds Duration;
    };

    using namespace std::chrono_literals;

    constexpr std::array BenchmarkStages{
        BenchmarkStage{"single_gpu_xess", ConfigureSingleGPUXeSS, 300s},
        BenchmarkStage{"dual_gpu_xess", ConfigureDualGPUXeSS, 300s}
    };
}

class BenchmarkApp final : public App
{
public:
    BenchmarkApp(HINSTANCE hInstance);
    BenchmarkApp(const BenchmarkApp& rhs) = delete;
    BenchmarkApp& operator =(const BenchmarkApp& rhs) = delete;
    ~BenchmarkApp() override;

    bool Initialize() override;

protected:
    void OnResize() override;

    void OnMouseDown(WPARAM btnState, int x, int y) override;
    void OnMouseUp(WPARAM btnState, int x, int y) override;
    void OnMouseMove(WPARAM btnState, int x, int y) override;

private:
    void AdvanceBenchmark();

    size_t _stageIndex = 0;
    ListenerHandle _runCompletedListener = 0;
};

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, PSTR, int)
{
    // Enable run-time memory check for debug builds.
#if defined(DEBUG) | defined(_DEBUG)
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif

    try
    {
        BenchmarkApp TheApp(hInstance);
        if (!TheApp.Initialize())
        {
            return 1;
        }

        return TheApp.Run();
    }
    catch (DxException& e)
    {
        const auto message = e.ToString();
        std::fwprintf(stderr, L"%ls\n", message.c_str());
        OutputDebugStringW(message.c_str());
        return 1;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "%s\n", e.what());
        OutputDebugStringA(e.what());
        return 1;
    }
}

BenchmarkApp::BenchmarkApp(HINSTANCE hInstance)
    : App(hInstance)
{
}

BenchmarkApp::~BenchmarkApp()
{
    if (_runCompletedListener)
    {
        GetLocator().GetModule<RenderModule>()->OnBenchmarkRunCompleted().RemoveListener(_runCompletedListener);
    }
}

bool BenchmarkApp::Initialize()
{
    if (!App::Initialize())
    {
        return false;
    }
    const auto& stage = BenchmarkStages.front();
    auto render = GetLocator().GetModule<RenderModule>();
    render->SetGraphicsPipeline(stage.Configuration);
    _runCompletedListener = render->OnBenchmarkRunCompleted().AddListener([this]()
    {
        AdvanceBenchmark();
    });
    render->StartBenchmarkRun(stage.Name, stage.Duration);
    return true;
}

void BenchmarkApp::OnResize()
{
    App::OnResize();
}

void BenchmarkApp::AdvanceBenchmark()
{
    if (++_stageIndex == BenchmarkStages.size())
    {
        PostQuitMessage(0);
        return;
    }
    const auto& stage = BenchmarkStages[_stageIndex];
    auto render = GetLocator().GetModule<RenderModule>();
    render->SetGraphicsPipeline(stage.Configuration);
    render->StartBenchmarkRun(stage.Name, stage.Duration);
}

void BenchmarkApp::OnMouseDown(WPARAM btnState, int x, int y)
{
    UNREFERENCED_PARAMETER(btnState);
    UNREFERENCED_PARAMETER(x);
    UNREFERENCED_PARAMETER(y);

    SetCapture(GetWindow()->GetWindowHandle());
}

void BenchmarkApp::OnMouseUp(WPARAM btnState, int x, int y)
{
    UNREFERENCED_PARAMETER(btnState);
    UNREFERENCED_PARAMETER(x);
    UNREFERENCED_PARAMETER(y);

    ReleaseCapture();
}

void BenchmarkApp::OnMouseMove(WPARAM btnState, int x, int y)
{
    UNREFERENCED_PARAMETER(btnState);
    UNREFERENCED_PARAMETER(x);
    UNREFERENCED_PARAMETER(y);
}
