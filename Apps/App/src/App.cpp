#include <App.Base/App.h>
#include <cmath>
#include <cstdlib>
#include <stdexcept>

namespace
{
    class BistroApp final : public App
    {
    public:
        explicit BistroApp(const HINSTANCE instance) : App(instance) {}

    protected:
        bool AddModules() override
        {
            const auto scenePath = std::filesystem::path(ASSETS_FOLDER) / "Scenes" / "Amazon Lumberyard Bistro";
            for (const auto* part : { L"Exterior/exterior.obj", L"Interior/interior.obj" })
            {
                if (!std::filesystem::is_regular_file(scenePath / part))
                {
                    throw std::runtime_error("Missing Bistro scene part: " + (scenePath / part).string());
                }
            }

            Locator.RegisterModule(std::make_shared<RenderModule>(GetWindow(), &Timer));
            Locator.RegisterModule(std::make_shared<SceneManagerModule>(&Timer, scenePath));
            return App::AddModules();
        }

        void OnMouseDown(WPARAM, int x, int y) override
        {
            _lastMousePos = { x, y };
            SetCapture(GetWindow()->GetWindowHandle());
        }

        void OnMouseUp(WPARAM, int, int) override
        {
            ReleaseCapture();
        }

        void OnMouseMove(WPARAM buttons, int x, int y) override
        {
            if (buttons & MK_RBUTTON)
            {
                auto* world = Locator.GetModule<SceneManagerModule>()->GetWorld();
                auto& camera = world->GetECS().Get<TransformComponent>(world->ActiveCamera);
                camera.Rotation.x = std::clamp(camera.Rotation.x + (y - _lastMousePos.y) * 0.15f, -89.0f, 89.0f);
                camera.Rotation.y = std::remainder(camera.Rotation.y - (x - _lastMousePos.x) * 0.15f, 360.0f);
                camera.DirtyFlag = true;
            }
            _lastMousePos = { x, y };
        }

        void OnMouseWheelMove(WPARAM rotation) override
        {
            _cameraSpeed = std::clamp(_cameraSpeed * (GET_WHEEL_DELTA_WPARAM(rotation) > 0 ? 1.25f : 0.8f), 0.1f, 10000.0f);
        }

        void OnKeyboardInput(const GameTimer& timer) override
        {
            auto* world = Locator.GetModule<SceneManagerModule>()->GetWorld();
            auto& camera = world->GetECS().Get<TransformComponent>(world->ActiveCamera);
            const auto rotation = Matrix::CreateFromYawPitchRoll(
                XMConvertToRadians(camera.Rotation.y), XMConvertToRadians(camera.Rotation.x), 0.0f);
            Vector3 movement = Vector3::Zero;
            if (GetAsyncKeyState('W') & 0x8000) movement -= rotation.Forward();
            if (GetAsyncKeyState('S') & 0x8000) movement += rotation.Forward();
            if (GetAsyncKeyState('A') & 0x8000) movement += rotation.Right();
            if (GetAsyncKeyState('D') & 0x8000) movement -= rotation.Right();
            if (GetAsyncKeyState('Q') & 0x8000) movement -= Vector3::Up;
            if (GetAsyncKeyState('E') & 0x8000) movement += Vector3::Up;
            if (movement.LengthSquared() > 0.0f)
            {
                movement.Normalize();
                const float boost = (GetAsyncKeyState(VK_SHIFT) & 0x8000) ? 4.0f : 1.0f;
                camera.Location += movement * _cameraSpeed * boost * timer.DeltaTime();
                camera.DirtyFlag = true;
            }
        }

    private:
        POINT _lastMousePos = {};
        float _cameraSpeed = 100.0f;
    };
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, PSTR, int)
{
#if defined(DEBUG) || defined(_DEBUG)
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif
    try
    {
        BistroApp app(instance);
        if (!app.Initialize()) return EXIT_FAILURE;
        return app.Run();
    }
    catch (const DxException& error)
    {
        MessageBoxW(nullptr, error.ToString().c_str(), L"Bistro App", MB_OK | MB_ICONERROR);
    }
    catch (const std::exception& error)
    {
        MessageBoxA(nullptr, error.what(), "Bistro App", MB_OK | MB_ICONERROR);
    }
    return EXIT_FAILURE;
}
