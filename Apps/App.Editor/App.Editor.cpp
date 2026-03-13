#include <App.Base/AppBase.h>

class EditorApp : public AppBase
{
public:
	EditorApp(HINSTANCE hInstance);
	EditorApp(const EditorApp& rhs) = delete;
	EditorApp& operator = (const EditorApp& rhs) = delete;
	~EditorApp();

	virtual bool Initialize() override;

private:
	virtual void OnResize() override;
	virtual void Update(const GameTimer& gameTimer) override;
	virtual void Render(const GameTimer& gameTimer) override;

	virtual void OnMouseDown(WPARAM btnState, int x, int y) override;
	virtual void OnMouseUp(WPARAM btnState, int x, int y) override;
	virtual void OnMouseMove(WPARAM btnState, int x, int y) override;

	std::unique_ptr<GDX12BackBuffer> testBackBuffer;
	std::shared_ptr<GDX12Device> defaultDevice;
	std::shared_ptr<GDX12DescriptorHeap> RTVHeap;
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
	: AppBase(hInstance)
{
}

EditorApp::~EditorApp()
{
}

bool EditorApp::Initialize()
{
	if (!AppBase::Initialize())
	{
		return false;
	}

	defaultDevice = std::make_shared<GDX12Device>();
	defaultDevice->Initialize(GDX12DeviceFactory::GetDefaultAdapter().Get());

	RTVHeap = std::make_shared<GDX12DescriptorHeap>(defaultDevice.get(),
		D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 20, D3D12_DESCRIPTOR_HEAP_FLAG_NONE);
	
	testBackBuffer = std::make_unique<GDX12BackBuffer>(defaultDevice, MainWndHandle, 
		DXGI_FORMAT_R8G8B8A8_UNORM, 2,
		WindowWidth, WindowHeight, RTVHeap);

	return true;
}

void EditorApp::OnResize()
{
	AppBase::OnResize();
}

void EditorApp::Update(const GameTimer& gameTimer)
{
	UNREFERENCED_PARAMETER(gameTimer);
}

void EditorApp::Render(const GameTimer& gameTimer)
{
	auto cmdQueue = defaultDevice->GetCommandQueue();

	cmdQueue->Flush();

	auto cmdList = cmdQueue->GetCommandList();
	auto backBuffer = testBackBuffer->GetCurrentBuffer();

	float clearColor[] = { 0.5 + 0.5 * cos(gameTimer.TotalTime()), 0.5 + 0.5 * sin(gameTimer.TotalTime()), 
		0.5 + 0.5 * cos(gameTimer.TotalTime()), 1.0f };

	cmdList->GetCommandList()->ClearRenderTargetView(backBuffer->GetRTV()->CPUHandle, clearColor, 0, nullptr);

	cmdQueue->ExecuteCommandList(cmdList);

	testBackBuffer->Present();
}

void EditorApp::OnMouseDown(WPARAM btnState, int x, int y)
{
	UNREFERENCED_PARAMETER(btnState);
	UNREFERENCED_PARAMETER(x);
	UNREFERENCED_PARAMETER(y);

	SetCapture(MainWndHandle);
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