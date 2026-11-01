// AppBase.h

#pragma once

#if defined(DEBUG) || defined(_DEBUG)
#define _CRTDBG_MAP_ALLOC
#include <crtdbg.h>
#endif

#include "Engine.Core/BenchmarkEngine.h"
#include "Engine.RendererDX12/D3DHelpers.h"

#include "App.Base/Modules/RenderModule.h"
#include "App.Base/Modules/SceneManagerModule.h"
#include "App.Base/Window.h"
#include "App.Base/Systems/MovementSystem.h"
#include "App.Base/Systems/CircleMovementSystem.h"
#include "App.Base/Systems/SplineFollowSystem.h"
#include "App.Base/Systems/LookAtTargetSystem.h"
#include "App.Base/Systems/LightUpdateSystem.h"
#include "App.Base/Systems/ReflectionProbeUpdateSystem.h"
#include "App.Base/Systems/SkyUpdateSystem.h"
#include "App.Base/Systems/GrassWindUpdateSystem.h"
#include "App.Base/Systems/GPUDataUpdateSystem.h"
#include "App.Base/Systems/IndirectCommandBuildSystem.h"

#undef min
#undef max

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
    JobSystem& GetJobs() { return _jobs; }
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

    virtual void OnMouseWheelMove(WPARAM rotation)
    {
    }

    //this should probably be replaced with an InputSystem
    virtual void OnKeyboardInput(const GameTimer& gameTimer)
    {
    }

    bool InitMainWindow();
    bool AddModules() override;
    void Update(const GameTimer& gameTimer) override;

    JobSystem _jobs;
    MovementSystem _movement;
    CircleMovementSystem _circle;
    SplineFollowSystem _spline;
    LookAtTargetSystem _lookAt;
    LightUpdateSystem _lights;
    GPUDataUpdateSystem _gpuData;
    ReflectionProbeUpdateSystem _reflectionProbes;
    SkyUpdateSystem _sky;
    GrassWindUpdateSystem _grassWind;
    IndirectCommandBuildSystem _indirect;

private:
    std::unique_ptr<Window> _window;
};
