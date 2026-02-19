#include <App.Base/AppBase.h>

class BenchmarkApp : public AppBase
{
public:
	BenchmarkApp(HINSTANCE hInstance);
	BenchmarkApp(const BenchmarkApp& rhs) = delete;
	BenchmarkApp& operator=(const BenchmarkApp& rhs) = delete;
	~BenchmarkApp();

	virtual bool Initialize()override;

private:
	virtual void OnResize()override;
	virtual void Update(const GameTimer& gt)override;
	virtual void Draw(const GameTimer& gt)override;

	virtual void OnMouseDown(WPARAM btnState, int x, int y)override;
	virtual void OnMouseUp(WPARAM btnState, int x, int y)override;
	virtual void OnMouseMove(WPARAM btnState, int x, int y)override;

};

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE prevInstance,
	PSTR cmdLine, int showCmd)
{
	// Enable run-time memory check for debug builds.
#if defined(DEBUG) | defined(_DEBUG)
	_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif

	try
	{
		BenchmarkApp theApp(hInstance);
		if (!theApp.Initialize())
			return 0;

		return theApp.Run();
	}
	catch (DxException& e)
	{
		MessageBox(nullptr, e.ToString().c_str(), L"App Init Failed", MB_OK);
		return 0;
	}
}

BenchmarkApp::BenchmarkApp(HINSTANCE hInstance)
	: AppBase(hInstance)
{
}

BenchmarkApp::~BenchmarkApp()
{

}

bool BenchmarkApp::Initialize()
{
	if (!AppBase::Initialize())
		return false;

	return true;
}

void BenchmarkApp::OnResize()
{
	AppBase::OnResize();

}

void BenchmarkApp::Update(const GameTimer& gt)
{
	
}

void BenchmarkApp::Draw(const GameTimer& gt)
{
	
}

void BenchmarkApp::OnMouseDown(WPARAM btnState, int x, int y)
{

	SetCapture(hMainWnd);
}

void BenchmarkApp::OnMouseUp(WPARAM btnState, int x, int y)
{
	ReleaseCapture();
}

void BenchmarkApp::OnMouseMove(WPARAM btnState, int x, int y)
{

}

