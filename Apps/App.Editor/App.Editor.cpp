#include <App.Base/App.h>

class EditorApp : public App
{
public:
    EditorApp(HINSTANCE hInstance);
    EditorApp(const EditorApp& rhs) = delete;
    EditorApp& operator =(const EditorApp& rhs) = delete;
    ~EditorApp() override;

    bool Initialize() override;

protected:
    void OnResize() override;
    void Update(const GameTimer& gameTimer) override;
    void Render(const GameTimer& gameTimer) override;

    void OnMouseDown(WPARAM btnState, int x, int y) override;
    void OnMouseUp(WPARAM btnState, int x, int y) override;
    void OnMouseMove(WPARAM btnState, int x, int y) override;
    void OnMouseWheelMove(WPARAM rotation) override;
    void OnKeyboardInput(const GameTimer& gameTimer) override;
    
    std::string GetAppConfigPath() const override
    {
        return "Configs/App.Editor/App.yaml";
    }

private:
    POINT _lastMousePos = {};
    float _cameraSpeed = 10.0f;
    float _minCameraSpeed = 0.1f;
    float _maxCameraSpeed = 10000.0f;
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
    : App(hInstance)
{
}

EditorApp::~EditorApp()
{
}

bool EditorApp::Initialize()
{
    if (!App::Initialize())
    {
        return false;
    }

    return true;
}

void EditorApp::OnResize()
{
    App::OnResize();
}

void EditorApp::Update(const GameTimer& gameTimer)
{
    App::Update(gameTimer);
}

void EditorApp::Render(const GameTimer& gameTimer)
{
    App::Render(gameTimer);
}

void EditorApp::OnMouseDown(WPARAM btnState, int x, int y)
{
    UNREFERENCED_PARAMETER(btnState);

    _lastMousePos.x = x;
    _lastMousePos.y = y;

    SetCapture(GetWindow()->GetWindowHandle());
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
    if ((btnState & MK_RBUTTON) != 0)
    {
        constexpr float mouseSensitivity = 0.15f;

        const float dx = static_cast<float>(x - _lastMousePos.x) * mouseSensitivity;
        const float dy = static_cast<float>(y - _lastMousePos.y) * mouseSensitivity;

        auto world = GetLocator().GetModule<SceneManagerModule>()->GetWorld();
        auto& camera = world->GetECS().Get<TransformComponent>(world->ActiveCamera);

        camera.Rotation.x += dy;
        camera.Rotation.y -= dx;

        if (camera.Rotation.x > 89.0f)
        {
            camera.Rotation.x = 89.0f;
        }
        if (camera.Rotation.x < -89.0f)
        {
            camera.Rotation.x = -89.0f;
        }

        if (camera.Rotation.y > 180.0f)
        {
            camera.Rotation.y -= 360.0f;
        }
        if (camera.Rotation.y < -180.0f)
        {
            camera.Rotation.y += 360.0f;
        }

        camera.DirtyFlag = true;
    }

    _lastMousePos.x = x;
    _lastMousePos.y = y;
}

void EditorApp::OnMouseWheelMove(WPARAM rotation)
{
    short wheelDelta = GET_WHEEL_DELTA_WPARAM(rotation);

    float speed = _cameraSpeed;
    if (wheelDelta > 0) speed = std::min(speed + 4.0f, _maxCameraSpeed);
    else if (wheelDelta < 0) speed = (std::max)(speed - 4.0f, _minCameraSpeed);

    _cameraSpeed = speed;
}

void EditorApp::OnKeyboardInput(const GameTimer& gameTimer)
{
    const float dt = gameTimer.DeltaTime();
    const float speed = _cameraSpeed;

    auto world = GetLocator().GetModule<SceneManagerModule>()->GetWorld();
    auto& camera = world->GetECS().Get<TransformComponent>(world->ActiveCamera);

    Matrix rotMatrix = Matrix::CreateFromYawPitchRoll(XMConvertToRadians(camera.Rotation.y), 
        XMConvertToRadians(camera.Rotation.x), XMConvertToRadians(camera.Rotation.z));

    //local space
    Vector3 forward = rotMatrix.Forward();
    Vector3 right = rotMatrix.Right();

    //world space
    Vector3 up = Vector3::Up;

    bool moved = false;

    if (GetAsyncKeyState('W') & 0x8000) { camera.Location -= forward * speed * dt; moved = true; }
    if (GetAsyncKeyState('S') & 0x8000) { camera.Location += forward * speed * dt; moved = true; }
    if (GetAsyncKeyState('A') & 0x8000) { camera.Location += right * speed * dt; moved = true; }
    if (GetAsyncKeyState('D') & 0x8000) { camera.Location -= right * speed * dt; moved = true; }
    if (GetAsyncKeyState('Q') & 0x8000) { camera.Location -= up * speed * dt * 0.5f; moved = true; }
    if (GetAsyncKeyState('E') & 0x8000) { camera.Location += up * speed * dt * 0.5f; moved = true; }

    if (moved)
    {
        camera.DirtyFlag = true;
    }
}
