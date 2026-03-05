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

	//GDX12DeviceFactory example
	std::vector<DeviceDesc> DeviceDescs = GDX12DeviceFactory::GetDeviceDescriptors();
	int k = 1;
	for (auto DeviceDesc : DeviceDescs)
	{
		std::wstring output = std::to_wstring(k) + L": " + DeviceDesc.Name + L", "
			+ std::to_wstring(DeviceDesc.DedicatedVideoMemory / (1024 * 1024)) + L" MB memory\n";
		OutputDebugStringW(output.c_str());
		k++;
	}

	//GDX12DeviceFactory example
	GDX12Device mGPUPrimaryDevice;
	GDX12Device mGPUSecondaryDevice;

	//adapters will be selected via UI
	mGPUPrimaryDevice.Initialize(DeviceDescs[0].Adapter.Get());
	mGPUSecondaryDevice.Initialize(DeviceDescs[1].Adapter.Get());

	std::vector<GDX12Device*> devices = { &mGPUPrimaryDevice, &mGPUSecondaryDevice };

	for (auto& device : devices)
	{
		const auto& specs = device->GetDeviceFeatures();

		std::string output = "\n========================================\n";
		output += specs.Name + "\n";
		output += "========================================\n";

		output += "Memory:\n";
		output += "  Dedicated Video Memory: " + std::to_string(specs.DedicatedVideoMemory / (1024 * 1024)) + " MB\n";
		output += "  Dedicated System Memory: " + std::to_string(specs.DedicatedSystemMemory / (1024 * 1024)) + " MB\n";
		output += "  Shared System Memory: " + std::to_string(specs.SharedSystemMemory / (1024 * 1024)) + " MB\n";
		output += "\nDirect3D Capabilities:\n";
		output += "  Max Feature Level: " + FeatureLevelToString(specs.MaxFeatureLevel) + "\n";
		output += "  Max Shader Model: " + ShaderModelToString(specs.MaxShaderModel) + "\n";
		output += "\nFeature Support:\n";
		output += "  Raytracing: " + std::string(specs.RaytracingSupport ? "Yes" : "No") + "\n";
		output += "  Mesh Shaders: " + std::string(specs.MeshShadersSupport ? "Yes" : "No") + "\n";
		output += "  Variable Rate Shading: " + std::string(specs.VariableRateShadingSupport ? "Yes" : "No") + "\n";
		output += "  Enhanced Barriers: " + std::string(specs.EnhancedBarriersSupport ? "Yes" : "No") + "\n";
		output += "========================================\n\n";

		OutputDebugStringA(output.c_str());
	}

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