#pragma once

#include <Windows.h>

class GDX12Texture;
class GDX12CommandList;
class GDX12DescriptorHeap;
class GDX12Device;

namespace Engine::UI
{
    enum class EditorMode
    {
        Edit,
        Play
    };

    struct EditorUIInitDesc
    {
        HWND WindowHandle = nullptr;
        GDX12Device* Device = nullptr;
        GDX12DescriptorHeap* SRVHeap = nullptr;
        UINT FrameCount = 3;
        //to not drag dependencies, it's converted to long
        long BackBufferFormat = 0;
    };
    
    void Initialize(const EditorUIInitDesc& initDesc);

    void ShutdownUI();

    // handler for forwarding messages to ImGui. returns true if Imgui handled the message
    LRESULT WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

    void BeginFrame();

    void BeginDockspace();
    void EndDockspace();

    void Render(GDX12CommandList* cmdList, GDX12Texture* backBuffer);

    bool WantCaptureMouse();
    bool WantCaptureKeyboard();

    EditorMode GetEditorMode();
    void SetEditorMode(EditorMode mode);
}


