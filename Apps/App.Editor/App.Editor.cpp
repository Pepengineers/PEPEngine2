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

	//Device creation example
	std::shared_ptr<GDX12Device> defaultDevice = std::make_shared<GDX12Device>();
	defaultDevice->Initialize(GDX12DeviceFactory::GetDefaultAdapter().Get());

	//Decriptor heap creation example
	std::shared_ptr<GDX12DescriptorHeap> SRVUAVHeap = std::make_shared<GDX12DescriptorHeap>(defaultDevice.get(), 
		D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 20, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
	std::shared_ptr<GDX12DescriptorHeap> RTVHeap = std::make_shared<GDX12DescriptorHeap>(defaultDevice.get(),
		D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 20, D3D12_DESCRIPTOR_HEAP_FLAG_NONE);
	std::shared_ptr<GDX12DescriptorHeap> DSVHeap = std::make_shared<GDX12DescriptorHeap>(defaultDevice.get(),
		D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 20, D3D12_DESCRIPTOR_HEAP_FLAG_NONE);
	
	//Texture creation example;
	GDX12TextureDesc desc;
	desc.DSVHeap = DSVHeap;
	desc.SRV_UAV_Heap = SRVUAVHeap;
	desc.RTVHeap = RTVHeap;

	desc.Width = WindowWidth;
	desc.Height = WindowHeight;
	desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	desc.ClearValue = { 0.f, 0.f, 0.f, 1.f };

	//Auto-built and allocated views
	desc.CreateSRV = true;
	desc.SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	desc.SRVDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	desc.SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	desc.SRVDesc.Texture2D.MipLevels = 1;
	desc.SRVDesc.Texture2D.MostDetailedMip = 0;

	desc.CreateRTV = true;
	desc.RTVDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	desc.RTVDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
	desc.RTVDesc.Texture2D.MipSlice = 0;
	desc.RTVDesc.Texture2D.PlaneSlice = 0;

	desc.CreateUAV = true;
	desc.UAVDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	desc.UAVDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
	desc.UAVDesc.Texture2D.MipSlice = 0;
	desc.UAVDesc.Texture2D.PlaneSlice = 0;

	std::unique_ptr<GDX12Texture> TestTexture = std::make_unique<GDX12Texture>(desc);

	//Precalculated view handles for simple use in command list:
	TestTexture->GetSRV()->GPUHandle;
	TestTexture->GetRTV()->CPUHandle;
	TestTexture->GetUAV()->GPUHandle;

	//Texture modification functions
	UINT NewWidth = 200;
	UINT NewHeight = 400;
	TestTexture->Resize(NewWidth, NewHeight);
	//TODO: Add data upload(image textures)

	//You can use same descriptor to make multiple textures
	desc.Format = desc.RTVDesc.Format = desc.SRVDesc.Format = desc.UAVDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	std::unique_ptr<GDX12Texture> TestTexture2 = std::make_unique<GDX12Texture>(desc);

	// Depth Stencil creation example
	desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
	desc.CreateRTV = desc.CreateUAV = false;

	desc.SRVDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;

	desc.CreateDSV = true;
	desc.DSVDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
	desc.DSVDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
	desc.DSVDesc.Texture2D.MipSlice = 0;
	
	std::unique_ptr<GDX12Texture> TestDepthStencil = std::make_unique<GDX12Texture>(desc);
	TestDepthStencil->GetDSV()->CPUHandle;

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