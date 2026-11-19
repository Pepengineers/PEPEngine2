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
struct GDX12ReconstructionStatus;

namespace Engine::UI
{
    void Initialize(HWND window, GDX12Device* device, int framesInFlight, DXGI_FORMAT format);
    void BeginFrame();
    void DrawReconstructionStatus(const GDX12ReconstructionStatus&, unsigned renderWidth, unsigned renderHeight,
        unsigned outputWidth, unsigned outputHeight);
    void DrawWorldBox(const DirectX::BoundingBox& box, const DirectX::XMFLOAT4X4& viewProjection,
        const char* label, unsigned int color);
    void Render(GDX12CommandList* commands, GDX12Texture* target);
    void ShutdownUI();
    bool HandleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    bool WantsKeyboardInput();
    bool WantsMouseInput();
#ifdef _DEBUG
    bool DebugUIVisible();
#endif
}
