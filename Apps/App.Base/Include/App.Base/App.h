// AppBase.h

#pragma once

#if defined(DEBUG) || defined(_DEBUG)
#define _CRTDBG_MAP_ALLOC
#include <crtdbg.h>
#endif

#include "Engine.Core/BenchmarkEngine.h"
#include "Engine.RendererDX12/D3DHelpers.h"

// Link necessary d3d12 libraries.
#pragma comment(lib,"d3dcompiler.lib")
#pragma comment(lib, "D3D12.lib")
#pragma comment(lib, "dxgi.lib")

class RenderModule;
class Window;

class App : public BenchmarkEngine
{
public:
    HINSTANCE GetAppHandler() const;
    Window* GetWindow() const;
    int Run();
    bool Initialize() override;
    void CalculateFrameStats() const;

    LRESULT MsgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    static App* GetInstance();
protected:
    HINSTANCE AppHandler = nullptr;

    bool bAppPaused = false;
    bool bMinimized = false;
    bool bMaximized = false;
    bool bResizing = false;
    bool bFullscreenState = false;


    App(HINSTANCE hInstance);
    App(const App& rhs) = delete;
    App& operator =(const App& rhs) = delete;
    virtual ~App() override;

    virtual void OnResize();
    
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
    bool AddModules() override;
    void Update(const GameTimer& gameTimer) override;

private:
    std::unique_ptr<Window> _window;
};
