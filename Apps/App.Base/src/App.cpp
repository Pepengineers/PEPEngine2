// AppBase.cpp

#include <App.Base/App.h>
#include <WindowsX.h>
#include "App.Base/AppConfigLoader.h"
#include "Common/Logger.h"

#include <filesystem>
#include <string>

using Microsoft::WRL::ComPtr;
using namespace std;
using namespace DirectX;

namespace Private
{
    static LRESULT CALLBACK MainWndProc(const HWND hwnd, const UINT msg, const WPARAM wParam, const LPARAM lParam)
    {
        return App::GetInstance()->MsgProc(hwnd, msg, wParam, lParam);
    }
}


void App::CalculateFrameStats() const
{
    // Code computes the average frames per second, and also the
    // average time it takes to render one frame. These stats
    // are appended to the window caption bar.

    static int FrameCount = 0;
    static float TimeElapsed = 0.0f;

    FrameCount++;

    // Compute averages over one second period.
    if ((Timer.TotalTime() - TimeElapsed) >= 1.0f)
    {
        float Fps = static_cast<float>(FrameCount);
        float MsPerFrame = 1000.0f / Fps;

        std::wstring FpsString = std::to_wstring(Fps);
        std::wstring MsPerFrameString = std::to_wstring(MsPerFrame);

        std::wstring WindowText = _window->GetWindowTitle() +
            L"    fps: " + FpsString +
            L"   mspf: " + MsPerFrameString;

        SetWindowText(_window->GetWindowHandle(), WindowText.c_str());

        // Reset for next average.
        FrameCount = 0;
        TimeElapsed += 1.0f;
    }
}


App* App::GetInstance()
{
    return static_cast<App*>(Instance);
}

App::App(HINSTANCE hInstance)
    : AppHandler(hInstance)
{
    Instance = this;
}

App::~App() = default;

HINSTANCE App::GetAppHandler() const
{
    return AppHandler;
}

Window* App::GetWindow() const
{
    return _window.get();
}

int App::Run()
{
    MSG Msg = {nullptr};

    Timer.Reset();

    while (Msg.message != WM_QUIT)
    {
        // If there are Window messages then process them.
        if (PeekMessage(&Msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&Msg);
            DispatchMessage(&Msg);
        }
        // Otherwise, do animation/game stuff.
        else
        {
            Timer.Tick();

            if (!bAppPaused)
            {
                Update(Timer);
                Render(Timer);
                CalculateFrameStats();
            }
            else
            {
                Sleep(100);
            }
        }
    }

    return static_cast<int>(Msg.wParam);
}

bool App::Initialize()
{
    Logger::Init();
    
    try {
        _AppConfig = AppConfigLoader::LoadAppConfig(GetAppConfigPath());
    }
    catch (const std::exception& exception) {
        Logger::Error("LoadAppConfig failed: {}", exception.what());
        return false;
    }

    Logger::Info("App::Initialize after LoadAppConfig.\nStartScene = {}", _AppConfig.StartScene);

    if (_AppConfig.StartScene.empty())
    {
        Logger::Error("App::Initialize failed: App config StartScene is empty.");
        return false;
    }

    if (!InitWindowClass())
    {
        return false;
    }

    if (!InitMainWindow())
    {
        return false;
    }

    Logger::Info("App::Initialize before BenchmarkEngine::Initialize");

    if (!BenchmarkEngine::Initialize())
    {
        return false;
    }

    Logger::Info("App::Initialize before LoadStartScene");

    if (!LoadStartScene())
    {
        return false;
    }

    return true;
}

LRESULT App::MsgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    // WM_ACTIVATE is sent when the window is activated or deactivated.
    // We pause the game when the window is deactivated and unpause it
    // when it becomes active.
    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE)
        {
            bAppPaused = true;
            Timer.Stop();
        }
        else
        {
            bAppPaused = false;
            Timer.Start();
        }
        return 0;

    // WM_SIZE is sent when the user resizes the window.
    case WM_SIZE:
        {
            // Save the new client area dimensions.
            if (_window == nullptr)
            {
                return 0;
            }

            auto newWindowWidth = LOWORD(lParam);
            auto newWindowHeight = HIWORD(lParam);
            _window->SetWindowSize(newWindowWidth, newWindowHeight);

            if (wParam == SIZE_MINIMIZED)
            {
                bAppPaused = true;
                bMinimized = true;
                bMaximized = false;
            }
            else if (wParam == SIZE_MAXIMIZED)
            {
                bAppPaused = false;
                bMinimized = false;
                bMaximized = true;
                OnResize();
            }
            else if (wParam == SIZE_RESTORED)
            {
                // Restoring from minimized state?
                if (bMinimized)
                {
                    bAppPaused = false;
                    bMinimized = false;
                    OnResize();
                }

                // Restoring from maximized state?
                else if (bMaximized)
                {
                    bAppPaused = false;
                    bMaximized = false;
                    OnResize();
                }
                else if (bResizing)
                {
                    // If user is dragging the resize bars, we do not resize
                    // the buffers here because as the user continuously
                    // drags the resize bars, a stream of WM_SIZE messages are
                    // sent to the window, and it would be pointless (and slow)
                    // to resize for each WM_SIZE message received from dragging
                    // the resize bars. So instead, we reset after the user is
                    // done resizing the window and releases the resize bars, which
                    // sends a WM_EXITSIZEMOVE message.
                }
                else // API call such as SetWindowPos or mSwapChain->SetFullscreenState.
                {
                    OnResize();
                }
            }
            return 0;
        }
    // WM_EXITSIZEMOVE is sent when the user grabs the resize bars.
    case WM_ENTERSIZEMOVE:
        bAppPaused = true;
        bResizing = true;
        Timer.Stop();
        return 0;

    // WM_EXITSIZEMOVE is sent when the user releases the resize bars.
    // Here we reset everything based on the new window dimensions.
    case WM_EXITSIZEMOVE:
        bAppPaused = false;
        bResizing = false;
        Timer.Start();
        OnResize();
        return 0;

    // WM_DESTROY is sent when the window is being destroyed.
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    // The WM_MENUCHAR message is sent when a menu is active and the user presses
    // a key that does not correspond to any mnemonic or accelerator key.
    case WM_MENUCHAR:
        // Don't beep when we alt-enter.
        return MAKELRESULT(0, MNC_CLOSE);

    // Catch this message so to prevent the window from becoming too small.
    case WM_GETMINMAXINFO:
        ((MINMAXINFO*)lParam)->ptMinTrackSize.x = 200;
        ((MINMAXINFO*)lParam)->ptMinTrackSize.y = 200;
        return 0;

    case WM_LBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_RBUTTONDOWN:
        OnMouseDown(wParam, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;

    case WM_LBUTTONUP:
    case WM_MBUTTONUP:
    case WM_RBUTTONUP:
        OnMouseUp(wParam, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;

    case WM_MOUSEMOVE:
        OnMouseMove(wParam, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;

    case WM_MOUSEWHEEL:
        OnMouseWheelMove(wParam);
        return 0;

    case WM_KEYUP:
        if (wParam == VK_ESCAPE)
        {
            PostQuitMessage(0);
        }
        return 0;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

void App::OnResize()
{
    //WinApi unintentionally calls OnResize() once during window creation.
    //It happens during AppBase::Initialize(), 
    //so none of the systems are currently initialized.
    //This effectively ignores the first OnResize() call.

    auto RenderSystem = Locator.GetModule<RenderModule>();
    if (!RenderSystem) { return; }

    RenderSystem->OnResize();
}

bool App::InitWindowClass()
{
    WNDCLASS WindowClass = {};
    WindowClass.style = CS_HREDRAW | CS_VREDRAW;
    WindowClass.lpfnWndProc = Private::MainWndProc;
    WindowClass.cbClsExtra = 0;
    WindowClass.cbWndExtra = 0;
    WindowClass.hInstance = AppHandler;
    WindowClass.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    WindowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    WindowClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
    WindowClass.lpszMenuName = nullptr;
    WindowClass.lpszClassName = L"MainWnd";

    if (!RegisterClass(&WindowClass))
    {
        Logger::Error("RegisterClass failed.");
        return false;
    }

    return true;
}

bool App::LoadStartScene()
{
    auto sceneManager = Locator.GetModule<SceneManagerModule>();

    if (!sceneManager)
    {
        Logger::Error("SceneManagerModule not found.");
        return false;
    }

    return sceneManager->LoadScene(_AppConfig.StartScene, _AppConfig);
}

bool App::InitMainWindow()
{
    _window = std::make_unique<Window>(1280, 800, AppHandler);
    return _window->Initialize();
}

bool App::AddModules()
{
    Locator.RegisterModule(std::make_shared<RenderModule>(_window.get(), &Timer));
    Locator.RegisterModule(std::make_shared<SceneManagerModule>(&Timer));
    return BenchmarkEngine::AddModules();
}

void App::Update(const GameTimer& gameTimer)
{
    OnKeyboardInput(gameTimer);
    //explicitly specifying update order since it is not the same
    // as the initialization order (that is used by the locator)
    Locator.GetModule<SceneManagerModule>()->Update();
    Locator.GetModule<RenderModule>()->Update();
}
