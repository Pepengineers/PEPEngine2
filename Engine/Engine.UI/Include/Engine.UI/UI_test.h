#pragma once

#include <Windows.h>
#include <dxgiformat.h>
#include <DirectXCollision.h>
#include <cstddef>
#include <string_view>
#ifdef _DEBUG
#include <initializer_list>
#include <string_view>
#endif

class GDX12Device;
class GDX12CommandList;
class GDX12Texture;

namespace Engine::UI
{
    void Initialize(HWND window, GDX12Device* device, int framesInFlight, DXGI_FORMAT format);
    void BeginFrame();
    void SuspendSettings(bool suspended);
    int DrawBistroQualityPanel(bool& open, std::string_view status, bool running, bool hasWinner,
        const char* const* techniques, const char* const* currentTiers, size_t techniqueCount, int& technique, int& tier);
#ifdef _DEBUG
    bool DebugControlsVisible();
    bool DebugButton(const char* label, std::initializer_list<std::string_view> lines = {});
#endif
    void DrawWorldBox(const DirectX::BoundingBox& box, const DirectX::XMFLOAT4X4& viewProjection,
        const char* label, unsigned int color);
    void Render(GDX12CommandList* commands, GDX12Texture* target);
    void ShutdownUI();
    bool HandleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    bool WantsKeyboardInput();
}
