#include <App.Base/App.h>

#include "Window.h"

class EditorApp : public App
{
public:
    EditorApp(HINSTANCE hInstance);
    EditorApp(const EditorApp& rhs) = delete;
    EditorApp& operator =(const EditorApp& rhs) = delete;
    ~EditorApp() override;

    bool Initialize() override;

protected:
    void OnResize() override;
    void Update(const GameTimer& gameTimer) override;
    void Render(const GameTimer& gameTimer) override;

    void OnMouseDown(WPARAM btnState, int x, int y) override;
    void OnMouseUp(WPARAM btnState, int x, int y) override;
    void OnMouseMove(WPARAM btnState, int x, int y) override;
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
        EditorApp TheApp(hInstance);
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

EditorApp::EditorApp(HINSTANCE hInstance)
    : App(hInstance)
{
}

EditorApp::~EditorApp()
{
}

bool EditorApp::Initialize()
{
    if (!App::Initialize())
    {
        return false;
    }

    return true;
}

void EditorApp::OnResize()
{
    App::OnResize();
}

void EditorApp::Update(const GameTimer& gameTimer)
{
    App::Update(gameTimer);
}

void EditorApp::Render(const GameTimer& gameTimer)
{
    App::Render(gameTimer);
}

void EditorApp::OnMouseDown(WPARAM btnState, int x, int y)
{
    UNREFERENCED_PARAMETER(btnState);
    UNREFERENCED_PARAMETER(x);
    UNREFERENCED_PARAMETER(y);

    SetCapture(GetWindow()->GetWindowHandle());
}

void EditorApp::OnMouseUp(WPARAM btnState, int x, int y)
{
    UNREFERENCED_PARAMETER(btnState);
    UNREFERENCED_PARAMETER(x);
    UNREFERENCED_PARAMETER(y);

    ReleaseCapture();
}

void EditorApp::OnMouseMove(WPARAM btnState, int x, int y)
{
    UNREFERENCED_PARAMETER(btnState);
    UNREFERENCED_PARAMETER(x);
    UNREFERENCED_PARAMETER(y);
}
