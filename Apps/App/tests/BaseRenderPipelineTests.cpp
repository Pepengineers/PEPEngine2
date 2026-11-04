#include "App.Base/App.h"
#include <d3d12sdklayers.h>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string_view>

static void Require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

class ApplicationClearPass final : public GDX12RenderPass
{
public:
    void RecordRenderGraph(GDX12RenderGraphBuilder& graph) override
    {
        graph.SetName("Application clear");
        graph.ReadWrite(_inputs[0], D3D12_RESOURCE_STATE_RENDER_TARGET);
        graph.HasSideEffects();
    }
    void Execute(GDX12CommandList* commands) override
    {
        commands->ClearRenderTargetView(_inputs[0]->GetTexture());
    }
};

class ApplicationPipeline final : public IRenderPipeline
{
public:
    void Initialize(RenderPipelineContext& context) override
    {
        _clear.Setup(&context.Primary, &context.Secondary, &context.Common);
        Resize(context);
    }
    bool PrepareFrame(RenderPipelineContext&, GDX12CommandList*) override { return false; }
    void BuildGraph(GDX12RenderGraph& graph, const RenderPipelineGraphInputs& inputs) override
    {
        _clear.SetInputs({ inputs.BackBuffer });
        graph.AddPass(_clear);
    }
    void Resize(RenderPipelineContext& context) override
    {
        context.Common.DownscaledWidth = context.Common.WindowWidth;
        context.Common.DownscaledHeight = context.Common.WindowHeight;
    }
    const RenderPipelineBindings& GetBindings() const override { return _bindings; }
private:
    ApplicationClearPass _clear;
    RenderPipelineBindings _bindings{ { &_clear } };
};

class BaseRenderApp final : public App
{
public:
    explicit BaseRenderApp(bool custom) : App(GetModuleHandleW(nullptr)), _world(WorldDesc{}), _custom(custom) {}
    ~BaseRenderApp() override
    {
        if (_subscribed) Locator.GetModule<RenderModule>()->UnsubscribeFromWorld(_world);
    }
    void Check()
    {
        auto renderer = Locator.GetModule<RenderModule>();
        if (!_custom)
        {
            renderer->SubscribeToWorld(_world);
            _subscribed = true;
            auto camera = _world.GetECS().CreateEntity();
            camera.AddComponent<TransformComponent>();
            renderer->SetActiveCamera(&camera.AddComponent<CameraComponent>());
        }
        ComPtr<ID3D12Device> device;
        ThrowIfFailed(renderer->GetCurrentPrimaryFrameConstants()->MainCB->GetResource().D3DResource->GetDevice(IID_PPV_ARGS(&device)));
        ComPtr<ID3D12InfoQueue> info;
        ThrowIfFailed(device.As(&info));
        Timer.Reset();
        Timer.Stop();
        const auto passCount = renderer->GetRenderGraphStatistics().Passes;
        for (int frame = 0; frame < 3; ++frame)
        {
            renderer->BeginFrame();
            renderer->Update();
            if (!_custom) _gpuData.Schedule(_jobs, _world, 0).Complete();
            renderer->Render();
        }
        renderer->OnResize();
        Require(renderer->GetRenderGraphStatistics().Passes == passCount, "Resize duplicated render passes");
        for (UINT64 index = 0; index < info->GetNumStoredMessages(); ++index)
        {
            SIZE_T size = 0;
            ThrowIfFailed(info->GetMessage(index, nullptr, &size));
            std::vector<BYTE> bytes(size);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
            ThrowIfFailed(info->GetMessage(index, message, &size));
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) throw std::runtime_error(message->pDescription);
        }
        std::ostringstream graph;
        renderer->WriteRenderGraph(graph);
        const auto text = graph.str();
        if (_custom)
        {
            Require(passCount == 1 && text.find("Application clear") != std::string::npos,
                "Runtime changed the selected application composition");
            bool rejected = false;
            try { renderer->GetReconstructionStatus(); } catch (const std::logic_error&) { rejected = true; }
            Require(rejected, "Minimal pipeline falsely reported reconstruction support");
            rejected = false;
            try { renderer->GetFrameGenerationStatus(); } catch (const std::logic_error&) { rejected = true; }
            Require(rejected, "Minimal pipeline falsely reported frame generation support");
            std::cout << "Independent application pipeline GPU frames, resize and absent capabilities passed.\n";
            return;
        }
        Require(text.find("Fluid") == std::string::npos && text.find("Grass") == std::string::npos,
            "Base renderer created application effects");
        Require(text.find("Transparency") != std::string::npos && text.find("Particles") != std::string::npos,
            "Base renderer lost its common transparency or particles");
        std::cout << "Default pipeline empty-scene GPU frames, resize and D3D12 validation passed.\n";
        for (const auto* variable : ConsoleModule{}.GetConsoleVariables())
        {
            const auto name = variable->GetName();
            if (name.find(L"Render.Grass.") == 0 || name.find(L"Render.Wind.") == 0 || name.find(L"Render.Fluid.") == 0)
                throw std::runtime_error("Base consumer linked Bistro CVar: " + WStringToString(std::wstring(name)));
        }
        std::cout << "Base-only configuration, default-pipeline GPU frames and common particles passed.\n";
    }
protected:
    bool AddModules() override
    {
        if (!_custom) return App::AddModules();
        Locator.RegisterModule(std::make_shared<RenderModule>(GetWindow(), &Timer,
            std::make_unique<ApplicationPipeline>()));
        Locator.RegisterModule(std::make_shared<SceneManagerModule>());
        return BenchmarkEngine::AddModules();
    }
private:
    World _world;
    bool _subscribed = false;
    bool _custom;
};

int main(int argc, char** argv)
{
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    try
    {
        ComPtr<ID3D12Debug> debug;
        ThrowIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
        debug->EnableDebugLayer();
        BaseRenderApp app(argc > 1 && std::string_view(argv[1]) == "--custom");
        Require(app.Initialize(), "Base application initialization failed");
        ShowWindow(app.GetWindow()->GetWindowHandle(), SW_HIDE);
        app.Check();
        return 0;
    }
    catch (const DxException& error) { std::wcerr << error.ToString() << '\n'; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
    return 1;
}
