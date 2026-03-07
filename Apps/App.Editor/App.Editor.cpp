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

	std::shared_ptr<GDX12Device> defaultDeivce = std::make_shared<GDX12Device>();
	defaultDeivce->Initialize(GDX12DeviceFactory::GetDefaultAdapter().Get());


	DXGI_FORMAT BackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

	DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
	swapChainDesc.Width = WindowWidth;
	swapChainDesc.Height = WindowHeight;
	swapChainDesc.Format = BackBufferFormat;
	swapChainDesc.Stereo = FALSE;
	swapChainDesc.SampleDesc = { 1, 0 };
	swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	swapChainDesc.BufferCount = 2;
	swapChainDesc.Scaling = DXGI_SCALING_STRETCH;
	swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	swapChainDesc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;

	swapChainDesc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH | DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

	ComPtr<IDXGISwapChain4> mSwapChain = GDX12DeviceFactory::CreateSwapChain(defaultDeivce.get(), swapChainDesc, MainWndHandle);

	auto cmdList1 = defaultDeivce->GetCommandQueue()->GetCommandList();
	cmdList1->GetCommandList();
	defaultDeivce->GetCommandQueue()->ExecuteCommandList(cmdList1);

	auto cmdList2 = defaultDeivce->GetCommandQueue()->GetCommandList();
	auto cmdList3 = defaultDeivce->GetCommandQueue()->GetCommandList();

	cmdList2->GetCommandList();
	cmdList3->GetCommandList();

	std::shared_ptr<GDX12CommandList> lists[] = { cmdList1, cmdList2 };
	defaultDeivce->GetCommandQueue()->ExecuteCommandLists(lists, std::size(lists));

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
	UNREFERENCED_PARAMETER(gameTimer);
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