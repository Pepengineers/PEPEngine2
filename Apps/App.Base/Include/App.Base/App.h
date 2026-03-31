// AppBase.h

#pragma once

#if defined(DEBUG) || defined(_DEBUG)
#define _CRTDBG_MAP_ALLOC
#include <crtdbg.h>
#endif

#include <Engine.Core/GameTimer.h>

#include "ModuleLocator.h"
#include "Engine.RendererDX12/D3DHelpers.h"

// Link necessary d3d12 libraries.
#pragma comment(lib,"d3dcompiler.lib")
#pragma comment(lib, "D3D12.lib")
#pragma comment(lib, "dxgi.lib")

class RenderModule;
class Window;

class App
{
public:
    
    void CalculateFrameStats() const;
    static App* GetApp();
    HINSTANCE GetAppHandler() const;
    Window* GetWindow() const;
    ModuleLocator& GetLocator();
    int Run();
    virtual bool Initialize();

    LRESULT MsgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

protected:
    HINSTANCE AppHandler = nullptr;

    bool bAppPaused = false;
    bool bMinimized = false;
    bool bMaximized = false;
    bool bResizing = false;
    bool bFullscreenState = false;

    GameTimer Timer;

    App(HINSTANCE hInstance);
    App(const App& rhs) = delete;
    App& operator =(const App& rhs) = delete;
    virtual ~App();

    virtual void OnResize();
    virtual void Update(const GameTimer& gameTimer);
    virtual void Render(const GameTimer& gameTimer);

    // Convenience overrides for handling mouse input.
    virtual void OnMouseDown(WPARAM btnState, int x, int y)
    {
    }

    virtual void OnMouseUp(WPARAM btnState, int x, int y)
    {
    }

    virtual void OnMouseMove(WPARAM btnState, int x, int y)
    {
    }

    bool InitMainWindow();

private:
    ModuleLocator locator;
    static App* AppInstance;
    std::unique_ptr<Window> window;
};
