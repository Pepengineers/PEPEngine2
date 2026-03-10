// AppBase.h

#pragma once

#if defined(DEBUG) || defined(_DEBUG)
#define _CRTDBG_MAP_ALLOC
#include <crtdbg.h>
#endif

#include <Engine.Core/GameTimer.h>
#include <Engine.UI/UI_test.h>

//replace with #include "Engine.RHI/RenderingSystem.h" later
#include <Engine.RendererDX12/D3DHelpers.h>
#include <Engine.RendererDX12/dxc_test.h>
#include <Engine.RendererDX12/GDX12DeviceFactory.h>
#include <Engine.RendererDX12/GDX12Device.h>
#include <Engine.RendererDX12/GDX12CommandQueue.h>
#include <Engine.RendererDX12/GDX12CommandList.h>
#include <Engine.RendererDX12/GDX12RootSignature.h>

// Link necessary d3d12 libraries.
#pragma comment(lib,"d3dcompiler.lib")
#pragma comment(lib, "D3D12.lib")
#pragma comment(lib, "dxgi.lib")

class AppBase
{
public:
	static AppBase* GetApp ();

	HINSTANCE AppInst () const;
	HWND MainWnd () const;
	float AspectRatio () const;

	int Run ();
	virtual bool Initialize ();
	virtual LRESULT MsgProc (HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

protected:
	HINSTANCE AppInstance = nullptr;
	HWND MainWndHandle = nullptr;
	bool bAppPaused = false;
	bool bMinimized = false;
	bool bMaximized = false;
	bool bResizing = false;
	bool bFullscreenState = false;
	GameTimer Timer;
	std::wstring MainWndCaption = L"Renderer";
	int WindowWidth = 1280;
	int WindowHeight = 800;

	AppBase (HINSTANCE hInstance);
	AppBase (const AppBase& rhs) = delete;
	AppBase& operator = (const AppBase& rhs) = delete;
	virtual ~AppBase ();

	virtual void OnResize ();
	virtual void Update (const GameTimer& gameTimer) = 0;
	virtual void Render (const GameTimer& gameTimer) = 0;

	// Convenience overrides for handling mouse input.
	virtual void OnMouseDown (WPARAM btnState, int x, int y) { }
	virtual void OnMouseUp (WPARAM btnState, int x, int y) { }
	virtual void OnMouseMove (WPARAM btnState, int x, int y) { }

	bool InitMainWindow ();
	void CalculateFrameStats ();

private:
	static AppBase* _app;
};