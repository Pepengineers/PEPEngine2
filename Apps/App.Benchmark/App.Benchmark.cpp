// App.Benchmark.cpp

#include <App.Base/AppBase.h>

class BenchmarkApp : public AppBase
{
public:
	BenchmarkApp(HINSTANCE hInstance);
	BenchmarkApp(const BenchmarkApp& rhs) = delete;
	BenchmarkApp& operator =(const BenchmarkApp& rhs) = delete;
	~BenchmarkApp();

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
		BenchmarkApp TheApp(hInstance);
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
	{
		return false;
	}

	return true;
}

void BenchmarkApp::OnResize()
{
	AppBase::OnResize();
}

void BenchmarkApp::Update(const GameTimer& gameTimer)
{
	AppBase::Update(gameTimer);
}

void BenchmarkApp::Render(const GameTimer& gameTimer)
{
	AppBase::Render(gameTimer);
}

void BenchmarkApp::OnMouseDown(WPARAM btnState, int x, int y)
{
	UNREFERENCED_PARAMETER(btnState);
	UNREFERENCED_PARAMETER(x);
	UNREFERENCED_PARAMETER(y);

	SetCapture(MainWndHandle);
}

void BenchmarkApp::OnMouseUp(WPARAM btnState, int x, int y)
{
	UNREFERENCED_PARAMETER(btnState);
	UNREFERENCED_PARAMETER(x);
	UNREFERENCED_PARAMETER(y);

	ReleaseCapture();
}

void BenchmarkApp::OnMouseMove(WPARAM btnState, int x, int y)
{
	UNREFERENCED_PARAMETER(btnState);
	UNREFERENCED_PARAMETER(x);
	UNREFERENCED_PARAMETER(y);
}