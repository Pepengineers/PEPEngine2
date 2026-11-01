// AppBase.cpp

#include <App.Base/App.h>
#include <WindowsX.h>
#include "Common/ConsoleVariables.h"
#include "Engine.Core/ConsoleModule.h"
#include "Engine.Core/Logging/Logger.h"

#include "Engine.RendererDX12/GDX12StreamlineSDK.h"

using Microsoft::WRL::ComPtr;
using namespace std;
using namespace DirectX;

namespace Private
{
    static AutoConsoleVariable<int> JobThreadCount(
        L"Jobs.ThreadCount", 0, L"Total job threads including the caller; <= 0 uses hardware concurrency. Restart required.",
        EConsoleVariableFlags::ReadOnly);
    static AutoConsoleVariable<int> JobBatchSize(
        L"Jobs.BatchSize", 64, L"Default minimum elements per job batch; must be positive. Restart required.",
        EConsoleVariableFlags::ReadOnly, [](int value) { return value > 0; });

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
        const float elapsed = Timer.TotalTime() - TimeElapsed;
        float Fps = static_cast<float>(FrameCount) / elapsed;
        float MsPerFrame = 1000.0f / Fps;

        std::wstring FpsString = std::to_wstring(Fps);
        std::wstring MsPerFrameString = std::to_wstring(MsPerFrame);

        std::wstring WindowText = _window->GetWindowTitle() +
            L"    fps: " + FpsString +
            L"   mspf: " + MsPerFrameString;

        SetWindowText(_window->GetWindowHandle(), WindowText.c_str());

        // Reset for next average.
        FrameCount = 0;
        TimeElapsed = Timer.TotalTime();
    }
}


App* App::GetInstance()
{
    return dynamic_cast<App*>(Instance);
}

App::App(HINSTANCE hInstance)
    : AppHandler(hInstance), _jobs(static_cast<unsigned>((std::max)(0, Private::JobThreadCount.GetInt())),
        static_cast<size_t>(Private::JobBatchSize.GetInt()))
{
    Instance = this;
}

App::~App()
{
    Log::Info("Application", "Shutting down application resources");
    //ordering is important
    Locator.UnregisterModule<SceneManagerModule>();
    Locator.UnregisterModule<RenderModule>();
    // can't be put into RenderModule since it is required 
    // to call it after all module entities are destroyed
    GDX12StreamlineSDK::Get().Shutdown();
    Log::Info("Application", "Application resources released");
}

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

void App::ReportLoading(const std::wstring& stage)
{
    if (!_initializing) return;
    _loadingStage = stage;
    const auto title = L"Renderer - " + stage;
    SetWindowTextW(_window->GetWindowHandle(), title.c_str());
    InvalidateRect(_window->GetWindowHandle(), nullptr, FALSE);
    if (!ContinueLoading()) throw std::runtime_error("Scene loading cancelled");
}

bool App::ContinueLoading()
{
    if (GetCurrentThreadId() == _windowThread && _initializing)
    {
        MSG message{};
        while (PeekMessage(&message, nullptr, 0, 0, PM_REMOVE))
        {
            if (message.message == WM_QUIT) _loadingCancelled = true;
            else
            {
                TranslateMessage(&message);
                DispatchMessage(&message);
            }
        }
    }
    return !_loadingCancelled;
}

void App::WaitForLoading(const JobHandle& work)
{
    auto painted = GetTickCount64();
    // Leave the window thread available while the shared workers import/decode.
    if (_jobs.GetThreadCount() > 1)
        while (!work.IsCompleted())
        {
            ContinueLoading();
            if (GetTickCount64() - painted >= 1000)
            {
                InvalidateRect(_window->GetWindowHandle(), nullptr, FALSE);
                painted = GetTickCount64();
            }
            MsgWaitForMultipleObjectsEx(0, nullptr, 16, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        }
    work.Complete();
    if (!ContinueLoading()) throw std::runtime_error("Scene loading cancelled");
}

bool App::Initialize()
{
    Log::Info("Application", "Creating the application window");
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
        Log::Error("Application", "Window class registration failed, Win32 error ", GetLastError());
        MessageBox(nullptr, L"RegisterClass Failed.", nullptr, 0);
        return false;
    }

    if (!InitMainWindow())
    {
        Log::Error("Application", "Application window initialization failed");
        return false;
    }

    Log::Info("Application", "Registering application modules");
    if (!AddModules())
    {
        Log::Error("Application", "Application module registration failed");
        return false;
    }

    try
    {
        ReportLoading(L"Preparing renderer");
        // Scene loading creates GPU resources, so module hash order cannot decide initialization.
        Locator.GetModule<RenderModule>()->Initialize();
        ReportLoading(L"Importing scene");
        Locator.GetModule<SceneManagerModule>()->Initialize();
    }
    catch (...)
    {
        if (!_loadingCancelled) throw;
        Log::Info("Loading", "Scene loading cancelled by the user");
        return false;
    }
    Log::Info("Application", "Preparing initial indirect commands for ",
        Locator.GetModule<SceneManagerModule>()->GetWorldCount(), " worlds");
    auto scenes = Locator.GetModule<SceneManagerModule>();
    for (size_t i = 0; i < scenes->GetWorldCount(); ++i)
        _indirect.Schedule(_jobs, *scenes->GetWorld(i), 0).Complete();
    Log::Info("Application", "Application initialization complete");
    _initializing = false;
    bAppPaused = bMinimized;
    SetWindowTextW(_window->GetWindowHandle(), _window->GetWindowTitle().c_str());
    if (!bMinimized) OnResize();
    return true;
}

LRESULT App::MsgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (_initializing)
    {
        if (msg == WM_CLOSE || (msg == WM_KEYUP && wParam == VK_ESCAPE))
        {
            _loadingCancelled = true;
            _loadingStage = L"Cancelling loading...";
            SetWindowTextW(hwnd, _loadingStage.c_str());
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (msg == WM_PAINT)
        {
            PAINTSTRUCT paint{};
            const auto dc = BeginPaint(hwnd, &paint);
            RECT rect{};
            GetClientRect(hwnd, &rect);
            FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(230, 230, 230));
            const auto text = _loadingStage + L"\n" +
                std::to_wstring((GetTickCount64() - _loadingStarted) / 1000) + L" s - Esc to cancel";
            rect.top += 40;
            DrawTextW(dc, text.c_str(), -1, &rect, DT_CENTER | DT_TOP | DT_NOPREFIX);
            EndPaint(hwnd, &paint);
            return 0;
        }
        if (msg == WM_SIZE) { bMinimized = wParam == SIZE_MINIMIZED; return 0; }
        if (msg == WM_ACTIVATE || msg == WM_ENTERSIZEMOVE || msg == WM_EXITSIZEMOVE ||
            (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST)) return 0;
    }
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
            _window->UpdateClientSize(newWindowWidth, newWindowHeight);

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
    if (_initializing) return;
    uint16_t width, height;
    _window->GetWindowSize(width, height);
    _window->UpdateClientSize(width, height);
    //WinApi unintentionally calls OnResize() once during window creation.
    //It happens during AppBase::Initialize(), 
    //so none of the systems are currently initialized.
    //This effectively ignores the first OnResize() call.

    auto RenderSystem = Locator.GetModule<RenderModule>();
    if (!RenderSystem) { return; }

    RenderSystem->OnResize();
}

bool App::InitMainWindow()
{
    _window = std::make_unique<Window>(1280, 800, AppHandler);
    return _window->Initialize();
}

bool App::AddModules()
{
    Locator.RegisterModule(std::make_shared<RenderModule>(_window.get(), &Timer));
    Locator.RegisterModule(std::make_shared<SceneManagerModule>());
    return BenchmarkEngine::AddModules();
}

void App::Update(const GameTimer& gameTimer)
{
    OnKeyboardInput(gameTimer);
    Locator.GetModule<RenderModule>()->Update();
    auto scenes = Locator.GetModule<SceneManagerModule>();
    for (size_t i = 0; i < scenes->GetWorldCount(); ++i)
    {
        auto& world = *scenes->GetWorld(i);
        if (world.IsPaused())
        {
            _gpuData.Schedule(_jobs, world, 0).Complete();
            continue;
        }
        JobHandle work;
        try
        {
            const float dt = gameTimer.DeltaTime();
            work = _movement.Schedule(_jobs, world, dt);
            work = _circle.Schedule(_jobs, world, dt, work);
            work = _spline.Schedule(_jobs, world, dt, work);
            work = _lookAt.Schedule(_jobs, world, dt, work);
            work = _lights.Schedule(_jobs, world, dt, work);
            work = _gpuData.Schedule(_jobs, world, dt, work);
            work.Complete();
        }
        catch (...)
        {
            try { work.Complete(); } catch (...) {}
            throw;
        }
    }
}
