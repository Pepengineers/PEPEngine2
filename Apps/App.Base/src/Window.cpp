#include "App.Base/Window.h"

Window::Window(uint16_t width, uint16_t height, HINSTANCE hinstance) : WindowWidth(width), WindowHeight(height),
                                                                       AppInstance(hinstance)
{
}

bool Window::Initialize()
{
    // Compute window rectangle dimensions based on requested client area dimensions.
    RECT WindowRect = {0, 0, WindowWidth, WindowHeight};
    AdjustWindowRect(&WindowRect, WS_OVERLAPPEDWINDOW, false);
    int Width = WindowRect.right - WindowRect.left;
    int Height = WindowRect.bottom - WindowRect.top;

    MainWndHandle = CreateWindow(
        L"MainWnd",
        Title.c_str (),
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
        MessageBox(0, L"CreateWindow Failed.", nullptr, 0);
        return false;
    }

    ShowWindow(MainWndHandle, SW_SHOW);
    UpdateWindow(MainWndHandle);

    return true;
}

void Window::SetWindowSize(uint16_t width, uint16_t height)
{
    WindowWidth = width;
    WindowHeight = height == 0 ? 1 : height;

    if (!MainWndHandle)
    {
        return;
    }

    RECT WindowRect = {0, 0, WindowWidth, WindowHeight};
    AdjustWindowRect(&WindowRect, WS_OVERLAPPEDWINDOW, false);
    int Width = WindowRect.right - WindowRect.left;
    int Height = WindowRect.bottom - WindowRect.top;

    SetWindowPos(MainWndHandle, nullptr, 0, 0, Width, Height, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void Window::GetWindowSize(uint16_t& width, uint16_t& height) const
{
    if (MainWndHandle)
    {
        RECT ClientRect{};
        if (GetClientRect(MainWndHandle, &ClientRect))
        {
            width = static_cast<uint16_t>(ClientRect.right - ClientRect.left);
            height = static_cast<uint16_t>(ClientRect.bottom - ClientRect.top);
            return;
        }
    }

    width = WindowWidth;
    height = WindowHeight;
}

HWND Window::GetWindowHandle() const
{
    return MainWndHandle;
}

float Window::GetAspectRatio() const
{
    return WindowHeight == 0 ? 0.0f : static_cast<float>(WindowWidth) / WindowHeight;
}

const std::wstring& Window::GetWindowTitle() const
{
    return Title;
}
