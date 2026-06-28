// App.Benchmark.cpp

#include <Engine.Core/BenchmarkEngine.h>
#include <App.Base/App.h>

#include <Engine.Core/Registries/MeshRegistry.h>
#include <Engine.Core/Registries/TextureRegistry.h>
#include <Engine.Core/AssetManager.h>

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
    void Update(const GameTimer& gameTimer) override;
    void Render(const GameTimer& gameTimer) override;

    void OnMouseDown(WPARAM btnState, int x, int y) override;
    void OnMouseUp(WPARAM btnState, int x, int y) override;
    void OnMouseMove(WPARAM btnState, int x, int y) override;
    
    std::string GetAppConfigPath() const override
    {
        return "Configs/App.Benchmark/App.yaml";
    }
};

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE prevInstance, PSTR cmdLine, int showCmd)
{
    // Enable run-time memory check for debug builds.
#if defined(DEBUG) | defined(_DEBUG)
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif

    UNREFERENCED_PARAMETER(prevInstance);
    UNREFERENCED_PARAMETER(cmdLine);
    UNREFERENCED_PARAMETER(showCmd);

    try
    {
        BenchmarkApp TheApp(hInstance);
        if (!TheApp.Initialize())
        {
            return 0;
        }

        return TheApp.Run();
    }
    catch (DxException& e)
    {
        MessageBox(nullptr, e.ToString().c_str(), L"App Init Failed", MB_OK);
        return 0;
    }
}

BenchmarkApp::BenchmarkApp(HINSTANCE hInstance)
    : App(hInstance)
{
}

BenchmarkApp::~BenchmarkApp()
= default;

bool BenchmarkApp::Initialize()
{
    if (!App::Initialize())
    {
        return false;
    }

    return true;
}

void BenchmarkApp::OnResize()
{
    App::OnResize();
}

void BenchmarkApp::Update(const GameTimer& gameTimer)
{
    App::Update(gameTimer);
}

void BenchmarkApp::Render(const GameTimer& gameTimer)
{
    App::Render(gameTimer);
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