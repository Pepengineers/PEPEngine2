#pragma once

#include <Windows.h>
#include <dxgiformat.h>
#include <DirectXCollision.h>

class GDX12Device;
class GDX12CommandList;
class GDX12Texture;

namespace Engine::UI
{
    void Initialize(HWND window, GDX12Device* device, int framesInFlight, DXGI_FORMAT format);
    void BeginFrame();
#ifdef _DEBUG
    bool DebugButton(const char* label);
#endif
    void DrawWorldBox(const DirectX::BoundingBox& box, const DirectX::XMFLOAT4X4& viewProjection,
        const char* label, unsigned int color);
    void Render(GDX12CommandList* commands, GDX12Texture* target);
    void ShutdownUI();
    bool HandleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    bool WantsKeyboardInput();
}
