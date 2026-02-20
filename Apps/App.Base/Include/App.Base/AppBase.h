#pragma once

#if defined(DEBUG) || defined(_DEBUG)
#define _CRTDBG_MAP_ALLOC
#include <crtdbg.h>
#endif

#include <Engine.Core/GameTimer.h>
#include <Engine.RendererDX12/D3DHelpers.h>

// Link necessary d3d12 libraries.
#pragma comment(lib,"d3dcompiler.lib")
#pragma comment(lib, "D3D12.lib")
#pragma comment(lib, "dxgi.lib")

class AppBase
{
protected:

    AppBase(HINSTANCE hInstance);
    AppBase(const AppBase& rhs) = delete;
    AppBase& operator=(const AppBase& rhs) = delete;
    virtual ~AppBase();

public:

    static AppBase* GetApp();
    
	HINSTANCE AppInst()const;
	HWND      MainWnd()const;
	float     AspectRatio()const;

	int Run();
 
    virtual bool Initialize();
    virtual LRESULT MsgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

protected:
	virtual void OnResize(); 
	virtual void Update(const GameTimer& gt)=0;
    virtual void Draw(const GameTimer& gt)=0;

	// Convenience overrides for handling mouse input.
	virtual void OnMouseDown(WPARAM btnState, int x, int y){ }
	virtual void OnMouseUp(WPARAM btnState, int x, int y)  { }
	virtual void OnMouseMove(WPARAM btnState, int x, int y){ }

	bool InitMainWindow();

	void CalculateFrameStats();

    static AppBase* mApp;

    HINSTANCE hAppInst = nullptr; // application instance handle
    HWND      hMainWnd = nullptr; // main window handle
	bool      AppPaused = false;  // is the application paused?
	bool      Minimized = false;  // is the application minimized?
	bool      Maximized = false;  // is the application maximized?
	bool      Resizing = false;   // are the resize bars being dragged?
    bool      FullscreenState = false;// fullscreen enabled

	GameTimer Timer;

	// Startup Values
	std::wstring MainWndCaption = L"Renderer";
	int WindowWidth = 1280;
	int WindowHeight = 800;
};

