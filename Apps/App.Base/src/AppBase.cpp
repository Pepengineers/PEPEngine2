// AppBase.cpp

#include <App.Base/AppBase.h>
#include <WindowsX.h>

using Microsoft::WRL::ComPtr;
using namespace std;
using namespace DirectX;

LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	// Forward hwnd on because we can get messages (e.g., WM_CREATE)
	// before CreateWindow returns, and thus before MainWndHandle is valid.
	return AppBase::GetApp()->MsgProc (hwnd, msg, wParam, lParam);
}

AppBase* AppBase::_app = nullptr;

AppBase* AppBase::GetApp()
{
	return _app;
}

AppBase::AppBase(HINSTANCE hInstance)
	: AppInstance(hInstance)
{
	// Only one AppBase can be constructed.
	assert(_app == nullptr);
	_app = this;
}

AppBase::~AppBase ()
{
}

HINSTANCE AppBase::AppInst() const
{
	return AppInstance;
}

HWND AppBase::MainWnd() const
{
	return MainWndHandle;
}

float AppBase::AspectRatio() const
{
	return static_cast<float>(WindowWidth) / WindowHeight;
}

int AppBase::Run()
{
	MSG Msg = { 0 };

	Timer.Reset();

	while (Msg.message != WM_QUIT)
	{
		// If there are Window messages then process them.
		if (PeekMessage(&Msg, 0, 0, 0, PM_REMOVE))
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
				CalculateFrameStats();
				Update(Timer);
				Render(Timer);
			}
			else
			{
				Sleep(100);
			}
		}
	}

	return static_cast<int>(Msg.wParam);
}

bool AppBase::Initialize()
{
	if (!InitMainWindow())
	{
		return false;
	}

	RenderSystem = std::make_unique<RenderingSystem>();
	RenderSystem->Initialize(GDX12DeviceFactory::GetDefaultAdapter().Get(), nullptr,
		MainWndHandle, &Timer, WindowWidth, WindowHeight);

	return true;
}

void AppBase::Update(const GameTimer& gameTimer)
{
	RenderSystem->Update();
}

void AppBase::Render(const GameTimer& gameTimer)
{
	RenderSystem->Render();
}

void AppBase::OnResize()
{
	//WinApi unintentionally calls OnResize() once during window creation.
	//It happens during AppBase::Initialize(), 
	//so none of the systems are currently initialized.
	//This effectively ignores the first OnResize() call.
	if (!RenderSystem) return;

	RenderSystem->OnResize();
}

LRESULT AppBase::MsgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
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
			Timer.Stop ();
		}
		else
		{
			bAppPaused = false;
			Timer.Start ();
		}
		return 0;

	// WM_SIZE is sent when the user resizes the window.
	case WM_SIZE:
		// Save the new client area dimensions.
		WindowWidth = LOWORD(lParam);
		WindowHeight = HIWORD(lParam);
		if (RenderSystem) RenderSystem->SetWindowDimensions(WindowWidth, WindowHeight);
		if (true) // TODO if(DEVICE) HERE
		{
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
				OnResize ();
			}
			else if (wParam == SIZE_RESTORED)
			{
				// Restoring from minimized state?
				if (bMinimized)
				{
					bAppPaused = false;
					bMinimized = false;
					OnResize ();
				}

				// Restoring from maximized state?
				else if (bMaximized)
				{
					bAppPaused = false;
					bMaximized = false;
					OnResize ();
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
		}
		return 0;

	// WM_EXITSIZEMOVE is sent when the user grabs the resize bars.
	case WM_ENTERSIZEMOVE:
		bAppPaused = true;
		bResizing = true;
		Timer.Stop ();
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

	case WM_KEYUP:
		if (wParam == VK_ESCAPE)
		{
			PostQuitMessage(0);
		}
		return 0;
	}

	return DefWindowProc(hwnd, msg, wParam, lParam);
}

bool AppBase::InitMainWindow()
{
	WNDCLASS WindowClass;
	WindowClass.style = CS_HREDRAW | CS_VREDRAW;
	WindowClass.lpfnWndProc = MainWndProc;
	WindowClass.cbClsExtra = 0;
	WindowClass.cbWndExtra = 0;
	WindowClass.hInstance = AppInstance;
	WindowClass.hIcon = LoadIcon(0, IDI_APPLICATION);
	WindowClass.hCursor = LoadCursor(0, IDC_ARROW);
	WindowClass.hbrBackground = (HBRUSH)GetStockObject(NULL_BRUSH);
	WindowClass.lpszMenuName = 0;
	WindowClass.lpszClassName = L"MainWnd";

	if (!RegisterClass(&WindowClass))
	{
		MessageBox(0, L"RegisterClass Failed.", 0, 0);
		return false;
	}

	// Compute window rectangle dimensions based on requested client area dimensions.
	RECT WindowRect = { 0, 0, WindowWidth, WindowHeight };
	AdjustWindowRect(&WindowRect, WS_OVERLAPPEDWINDOW, false);
	int Width = WindowRect.right - WindowRect.left;
	int Height = WindowRect.bottom - WindowRect.top;

	MainWndHandle = CreateWindow(
		L"MainWnd",
		MainWndCaption.c_str (),
		WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT,
		CW_USEDEFAULT,
		Width,
		Height,
		0,
		0,
		AppInstance,
		0);
	if (!MainWndHandle)
	{
		MessageBox(0, L"CreateWindow Failed.", 0, 0);
		return false;
	}

	ShowWindow(MainWndHandle, SW_SHOW);
	UpdateWindow(MainWndHandle);

	return true;
}

void AppBase::CalculateFrameStats()
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

		wstring FpsString = to_wstring(Fps);
		wstring MsPerFrameString = to_wstring(MsPerFrame);

		wstring WindowText = MainWndCaption +
			L"    fps: " + FpsString +
			L"   mspf: " + MsPerFrameString;

		SetWindowText(MainWndHandle, WindowText.c_str ());

		// Reset for next average.
		FrameCount = 0;
		TimeElapsed += 1.0f;
	}
}