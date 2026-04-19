#pragma once
#include <string>

#include "App.Base/App.h"

class Window
{
public:
    Window(uint16_t width, uint16_t height, HINSTANCE hinstance);

    bool Initialize();

    void SetWindowSize(uint16_t width, uint16_t height);
    void GetWindowSize(uint16_t& width, uint16_t& height) const;

    HWND GetWindowHandle() const;

    float GetAspectRatio() const;
    
    const std::wstring& GetWindowTitle() const;

private:
    HWND MainWndHandle = nullptr;

    std::wstring Title = L"Renderer";
    uint16_t WindowWidth = 1280;
    uint16_t WindowHeight = 800;
    HINSTANCE AppInstance;
};
