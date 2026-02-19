#include <App.Base/AppBase.h>

class EditorApp : public AppBase
{
public:
	EditorApp(HINSTANCE hInstance);
	EditorApp(const EditorApp& rhs) = delete;
	EditorApp& operator=(const EditorApp& rhs) = delete;
	~EditorApp();

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
		EditorApp theApp(hInstance);
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
		return false;

	return true;
}

void EditorApp::OnResize()
{
	AppBase::OnResize();

}

void EditorApp::Update(const GameTimer& gt)
{

}

void EditorApp::Draw(const GameTimer& gt)
{

}

void EditorApp::OnMouseDown(WPARAM btnState, int x, int y)
{

	SetCapture(hMainWnd);
}

void EditorApp::OnMouseUp(WPARAM btnState, int x, int y)
{
	ReleaseCapture();
}

void EditorApp::OnMouseMove(WPARAM btnState, int x, int y)
{

}

