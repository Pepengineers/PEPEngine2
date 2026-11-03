#pragma once

#include <Windows.h>
#include <dxgiformat.h>

class GDX12Device;
class GDX12CommandList;
class GDX12Texture;

namespace Engine::UI
{
    void Initialize(HWND window, GDX12Device* device, int framesInFlight, DXGI_FORMAT format);
    void BeginFrame();
    void Render(GDX12CommandList* commands, GDX12Texture* target);
    void ShutdownUI();
    bool HandleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    bool WantsKeyboardInput();
}
