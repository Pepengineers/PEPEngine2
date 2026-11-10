#include <Engine.UI/EditorUI.h>

#include <memory>
#include <vector>

#include "Engine.RendererDX12/GDX12CommandList.h"
#include "Engine.RendererDX12/GDX12CommandQueue.h"
#include "Engine.RendererDX12/GDX12DescriptorHeap.h"
#include "Engine.RendererDX12/GDX12Device.h"
#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "imgui/backends/imgui_impl_dx12.h"
#include "imgui/backends/imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace Engine::UI
{
    namespace
    {
        GDX12Device* _device = nullptr;
        std::unique_ptr<GDX12DescriptorHeap> _srvHeap;
        std::vector<UINT> _freeSrvIndices;
    }

    void Initialize(const EditorUIInitDesc& initDesc)
    {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();

        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
        io.ConfigFlags |= ImGuiConfigFlags_DpiEnableScaleFonts;
        io.ConfigFlags |= ImGuiConfigFlags_DpiEnableScaleViewports;

        ImGui::StyleColorsDark();

        ImGuiStyle& style = ImGui::GetStyle();
        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
        {
            style.WindowRounding = 0.0f;
            style.Colors[ImGuiCol_WindowBg].w = 1.0f;
        }

        ImGui_ImplWin32_Init(initDesc.WindowHandle);

        _device = initDesc.Device;
        _srvHeap = std::make_unique<GDX12DescriptorHeap>(_device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                                                       64, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
        for (UINT i = _srvHeap->GetNumDescriptors(); i > 0; --i)
        {
            _freeSrvIndices.push_back(i - 1);
        }

        ImGui_ImplDX12_InitInfo initInfo = {};
        initInfo.Device = initDesc.Device->GetDevice().Get();
        initInfo.CommandQueue = initDesc.Device->GetCommandQueue()->GetCommandQueue().Get();
        initInfo.NumFramesInFlight = initDesc.FrameCount;
        initInfo.RTVFormat = static_cast<DXGI_FORMAT>(initDesc.BackBufferFormat);
        initInfo.DSVFormat = DXGI_FORMAT_UNKNOWN;
        initInfo.SrvDescriptorHeap = _srvHeap->GetHeap().Get();

        initInfo.UserData = nullptr;
        initInfo.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo*,
                                            D3D12_CPU_DESCRIPTOR_HANDLE* outCpuHandle,
                                            D3D12_GPU_DESCRIPTOR_HANDLE* outGpuHandle)
        {
            IM_ASSERT(!_freeSrvIndices.empty());
            const UINT index = _freeSrvIndices.back();
            _freeSrvIndices.pop_back();
            *outCpuHandle = _srvHeap->GetCPUHandle(index);
            *outGpuHandle = _srvHeap->GetGPUHandle(index);
        };

        initInfo.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo*,
                                            D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle,
                                            D3D12_GPU_DESCRIPTOR_HANDLE)
        {
            const SIZE_T stride = _srvHeap->GetCPUHandle(1).ptr - _srvHeap->GetCPUHandle(0).ptr;
            _freeSrvIndices.push_back(static_cast<UINT>((cpuHandle.ptr - _srvHeap->GetCPUHandle(0).ptr) / stride));
        };

        ImGui_ImplDX12_Init(&initInfo);
    }

    void ShutdownUI()
    {
        if (!ImGui::GetCurrentContext())
        {
            return;
        }

        _device->GetCommandQueue()->Flush();
        ImGui_ImplDX12_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        _srvHeap.reset();
        _freeSrvIndices.clear();
        _device = nullptr;
    }

    LRESULT WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        return ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);
    }

    void BeginFrame()
    {
        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
    }

    void BeginDockspace()
    {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::SetNextWindowViewport(viewport->ID);

        constexpr ImGuiDockNodeFlags dockFlags = ImGuiDockNodeFlags_PassthruCentralNode;

        ImGuiWindowFlags hostFlags = ImGuiWindowFlags_NoDocking
                                        | ImGuiWindowFlags_NoTitleBar
                                        | ImGuiWindowFlags_NoResize
                                        | ImGuiWindowFlags_NoMove
                                        | ImGuiWindowFlags_NoCollapse
                                        | ImGuiWindowFlags_NoBringToFrontOnFocus
                                        | ImGuiWindowFlags_NoNavFocus;

        if (dockFlags & ImGuiDockNodeFlags_PassthruCentralNode)
        {
            hostFlags |= ImGuiWindowFlags_NoBackground;
        }

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

        ImGui::Begin("EditorDockspace", nullptr, hostFlags);
        ImGui::PopStyleVar(3);

        const ImGuiID dockID = ImGui::GetID("HierarchyDockspace");
        if (!ImGui::DockBuilderGetNode(dockID))
        {
            ImGui::DockBuilderAddNode(dockID, ImGuiDockNodeFlags_DockSpace | dockFlags);
            ImGui::DockBuilderSetNodeSize(dockID, viewport->WorkSize);
            const ImGuiID leftDock = ImGui::DockBuilderSplitNode(dockID, ImGuiDir_Left, 0.22f, nullptr, nullptr);
            ImGui::DockBuilderDockWindow("Hierarchy", leftDock);
            ImGui::DockBuilderFinish(dockID);
        }
        ImGui::DockSpace(dockID, ImVec2(0.0f, 0.0f), dockFlags);
    }

    void EndDockspace()
    {
        ImGui::End();
    }

    void Render(GDX12CommandList* cmdList, GDX12Texture* backBuffer)
    {
        ImGui::Render();

        cmdList->SetRenderTargets({ backBuffer }, nullptr);

        ID3D12DescriptorHeap* heaps[] = {_srvHeap->GetHeap().Get()};
        cmdList->GetCommandList()->SetDescriptorHeaps(1, heaps);

        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cmdList->GetCommandList().Get());

        ImGuiIO& io = ImGui::GetIO();
        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
        {
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
        }
    }

    bool WantCaptureMouse()
    {
        return ImGui::GetIO().WantCaptureMouse;
    }

    bool WantCaptureKeyboard()
    {
        return ImGui::GetIO().WantCaptureKeyboard;
    }

}
