#include "Engine.UI/UI_test.h"
#include "Engine.Core/ConsoleModule.h"
#include "Engine.RendererDX12/GDX12CommandList.h"
#include "Engine.RendererDX12/GDX12CommandQueue.h"
#include "Engine.RendererDX12/GDX12DescriptorHeap.h"
#include "Engine.RendererDX12/GDX12Device.h"
#include "Engine.RendererDX12/GDX12Texture.h"
#include "Engine.RendererDX12/GDX12TextureResource.h"
#include <imgui/imgui.h>
#include <imgui/misc/imgui_stdlib.h>
#include <imgui/backends/imgui_impl_dx12.h>
#include <imgui/backends/imgui_impl_win32.h>
#include <filesystem>
#include <map>
#include <memory>
#include <charconv>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace Engine::UI
{
    namespace
    {
        std::unique_ptr<GDX12DescriptorHeap> Heap;
        std::vector<UINT> FreeDescriptors;
        bool Visible = true;
        std::string InvalidVariable;

        std::string Utf8(std::wstring_view text)
        {
            return std::filesystem::path(text).u8string();
        }

        template <class T>
        bool InputNumber(const char* label, ImGuiDataType type, std::string& value)
        {
            T number{};
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
                throw std::runtime_error("Cannot parse console variable for UI");
            const char* format = std::is_floating_point_v<T> ? "%.9g" : nullptr;
            if (!ImGui::InputScalar(label, type, &number, nullptr, nullptr, format))
                return false;
            char buffer[128];
            const auto result = std::to_chars(buffer, buffer + sizeof(buffer), number);
            if (result.ec != std::errc{}) throw std::runtime_error("Cannot format console variable from UI");
            value.assign(buffer, result.ptr);
            return true;
        }

        void DrawSettings()
        {
            if (!Visible) return;
            ImGui::SetNextWindowPos(ImVec2(16, 16), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(540, 650), ImGuiCond_FirstUseEver);
            if (ImGui::Begin("Settings (F1)", &Visible))
            {
                ImGui::TextWrapped("Changes apply live. Hover a setting for its CVar description and limits.");
                ImGui::TextWrapped("Session only. Startup settings are locked. F1 hides/shows this window.");
                static ImGuiTextFilter filter;
                filter.Draw("Search", -90);
                std::map<std::string, std::vector<IConsoleVariable*>> groups;
                for (auto* variable : ConsoleModule{}.GetConsoleVariables())
                {
                    const auto name = Utf8(variable->GetName());
                    const auto dot = name.rfind('.');
                    groups[dot == std::string::npos ? "General" : name.substr(0, dot)].push_back(variable);
                }
                for (const auto& [group, variables] : groups)
                {
                    bool matches = false;
                    for (auto* variable : variables)
                        matches |= filter.PassFilter(Utf8(variable->GetName()).c_str());
                    if (!matches || !ImGui::CollapsingHeader(group.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) continue;
                    for (auto* variable : variables)
                    {
                        const auto name = Utf8(variable->GetName());
                        if (!filter.PassFilter(name.c_str())) continue;
                        const auto dot = name.rfind('.');
                        const auto label = dot == std::string::npos ? name : name.substr(dot + 1);
                        auto value = variable->GetString();
                        bool changed = false;
                        const bool readOnly = variable->GetFlags() == EConsoleVariableFlags::ReadOnly;
                        ImGui::PushID(name.c_str());
                        ImGui::BeginDisabled(readOnly);
                        ImGui::SetNextItemWidth(210);
                        if (variable->GetType() == EConsoleVariableType::Boolean)
                        {
                            bool enabled = value == "true";
                            changed = ImGui::Checkbox(label.c_str(), &enabled);
                            value = enabled ? "1" : "0";
                        }
                        else if (variable->GetType() == EConsoleVariableType::Enumeration && variable->GetEnumCount() > 0)
                        {
                            if (ImGui::BeginCombo(label.c_str(), value.c_str()))
                            {
                                for (int index = 0; index < variable->GetEnumCount(); ++index)
                                {
                                    const auto item = std::to_string(index);
                                    const bool selected = item == value;
                                    if (ImGui::Selectable(item.c_str(), selected))
                                    { value = item; changed = true; }
                                    if (selected) ImGui::SetItemDefaultFocus();
                                }
                                ImGui::EndCombo();
                            }
                        }
                        else if (variable->GetType() == EConsoleVariableType::FloatingPoint)
                            changed = InputNumber<double>(label.c_str(), ImGuiDataType_Double, value);
                        else if (variable->GetType() == EConsoleVariableType::UnsignedInteger)
                            changed = InputNumber<uint64_t>(label.c_str(), ImGuiDataType_U64, value);
                        else if (variable->GetType() == EConsoleVariableType::Integer)
                            changed = InputNumber<int64_t>(label.c_str(), ImGuiDataType_S64, value);
                        else changed = ImGui::InputText(label.c_str(), &value, ImGuiInputTextFlags_EnterReturnsTrue);
                        ImGui::EndDisabled();
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                        {
                            ImGui::BeginTooltip();
                            ImGui::TextUnformatted(name.c_str());
                            ImGui::TextUnformatted(Utf8(variable->GetDescription()).c_str());
                            if (readOnly) ImGui::TextUnformatted("Restart required: edit Config/Engine.ini.");
                            ImGui::EndTooltip();
                        }
                        if (changed)
                            InvalidVariable = variable->SetFromString(value) ? std::string{} : name;
                        if (InvalidVariable == name)
                            ImGui::TextColored(ImVec4(1, 0.4f, 0.3f, 1), "Invalid value; previous value retained. See tooltip for limits.");
                        if (readOnly) { ImGui::SameLine(); ImGui::TextDisabled("(restart)"); }
                        ImGui::PopID();
                    }
                }
            }
            ImGui::End();
        }
    }

    void Initialize(HWND window, GDX12Device* device, int framesInFlight, DXGI_FORMAT format)
    {
        Heap = std::make_unique<GDX12DescriptorHeap>(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
            64, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
        FreeDescriptors.clear();
        for (UINT index = 0; index < Heap->GetNumDescriptors(); ++index) FreeDescriptors.push_back(index);
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        ImGui::StyleColorsDark();
        Visible = true;
        InvalidVariable.clear();
        try
        {
            if (!ImGui_ImplWin32_Init(window)) throw std::runtime_error("Cannot initialize ImGui Win32 backend");
            ImGui_ImplDX12_InitInfo info;
            info.Device = device->GetDevice().Get();
            info.CommandQueue = device->GetCommandQueue()->GetCommandQueue().Get();
            info.NumFramesInFlight = framesInFlight;
            info.RTVFormat = format;
            info.SrvDescriptorHeap = Heap->GetHeap().Get();
            info.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu)
            {
                if (FreeDescriptors.empty()) throw std::runtime_error("ImGui descriptor heap exhausted");
                const auto index = FreeDescriptors.back();
                FreeDescriptors.pop_back();
                *cpu = Heap->GetCPUHandle(index);
                *gpu = Heap->GetGPUHandle(index);
            };
            info.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE)
            {
                const auto increment = Heap->GetCPUHandle(1).ptr - Heap->GetCPUHandle(0).ptr;
                FreeDescriptors.push_back(static_cast<UINT>((cpu.ptr - Heap->GetCPUHandle(0).ptr) / increment));
            };
            if (!ImGui_ImplDX12_Init(&info)) throw std::runtime_error("Cannot initialize ImGui DX12 backend");
        }
        catch (...) { ShutdownUI(); throw; }
    }

    void BeginFrame()
    {
        if (!ImGui::GetCurrentContext()) return;
        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        DrawSettings();
        ImGui::Render();
    }

    void Render(GDX12CommandList* commands, GDX12Texture* target)
    {
        if (!ImGui::GetCurrentContext() || !ImGui::GetDrawData()) return;
        commands->ResourceBarrier({target->GetResource()->GetRenderTargetBarrier()});
        commands->SetRenderTargets({target}, nullptr);
        commands->SetDescriptorHeaps({Heap.get()});
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), commands->GetCommandList().Get());
        commands->InvalidateCachedState();
        commands->ResourceBarrier({target->GetResource()->GetPresentBarrier()});
    }

    void ShutdownUI()
    {
        if (!ImGui::GetCurrentContext()) return;
        if (ImGui::GetIO().BackendRendererUserData) ImGui_ImplDX12_Shutdown();
        if (ImGui::GetIO().BackendPlatformUserData) ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        Heap.reset();
        FreeDescriptors.clear();
    }

    bool HandleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (!ImGui::GetCurrentContext()) return false;
        const bool handled = ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam) != 0;
        if (message == WM_KEYUP && wParam == VK_F1) { Visible = !Visible; return true; }
        const auto& io = ImGui::GetIO();
        return handled || (Visible && ((message >= WM_MOUSEFIRST && message <= WM_MOUSELAST && io.WantCaptureMouse) ||
            (message >= WM_KEYFIRST && message <= WM_KEYLAST && io.WantCaptureKeyboard)));
    }

    bool WantsKeyboardInput()
    {
        return ImGui::GetCurrentContext() && Visible && ImGui::GetIO().WantCaptureKeyboard;
    }
}
