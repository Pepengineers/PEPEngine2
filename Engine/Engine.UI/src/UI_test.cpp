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
        bool FrameBegun = false;
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
                ImGui::TextWrapped("Changes are saved to Config/Engine.ini automatically. Hover a setting for its description and limits.");
                ImGui::TextWrapped("Readonly settings apply only after restarting the application. Other changes apply live. F1 hides/shows this window.");
                static ImGuiTextFilter filter;
                filter.Draw("Search", -90);
                struct Group
                {
                    std::map<std::string, Group> Children;
                    std::vector<IConsoleVariable*> Variables;
                };
                Group root;
                for (auto* variable : ConsoleModule{}.GetConsoleVariables())
                {
                    const auto name = Utf8(variable->GetName());
                    if (!filter.PassFilter(name.c_str())) continue;
                    auto* group = &root;
                    size_t start = 0;
                    for (auto dot = name.find('.'); dot != std::string::npos; dot = name.find('.', start))
                    {
                        group = &group->Children[name.substr(start, dot - start)];
                        start = dot + 1;
                    }
                    if (!start) group = &root.Children["General"];
                    group->Variables.push_back(variable);
                }
                const auto drawGroup = [&](auto&& self, const Group& group) -> void
                {
                    for (auto* variable : group.Variables)
                    {
                        const auto name = Utf8(variable->GetName());
                        const auto dot = name.rfind('.');
                        const auto label = dot == std::string::npos ? name : name.substr(dot + 1);
                        auto value = variable->GetConfigString();
                        bool changed = false;
                        const bool readOnly = variable->GetFlags() == EConsoleVariableFlags::ReadOnly;
                        ImGui::PushID(name.c_str());
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
                                    const auto item = std::to_string(variable->GetEnumFirstValue() + index);
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
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                        {
                            ImGui::BeginTooltip();
                            ImGui::TextUnformatted(name.c_str());
                            ImGui::TextUnformatted(Utf8(variable->GetDescription()).c_str());
                            if (readOnly)
                            {
                                ImGui::TextUnformatted("Readonly: changes are saved now and apply only after restarting the application.");
                                ImGui::Text("Active value: %s", variable->GetString().c_str());
                            }
                            ImGui::EndTooltip();
                        }
                        if (changed)
                            InvalidVariable = variable->SetConfigFromString(value) ? std::string{} : name;
                        if (InvalidVariable == name)
                            ImGui::TextColored(ImVec4(1, 0.4f, 0.3f, 1), "Invalid value or INI save failed; previous value retained. See tooltip and log.");
                        if (readOnly) { ImGui::SameLine(); ImGui::TextDisabled("(readonly)"); }
                        ImGui::PopID();
                    }
                    for (const auto& [name, child] : group.Children)
                    {
                        if (filter.IsActive()) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
                        if (ImGui::TreeNodeEx(name.c_str(), ImGuiTreeNodeFlags_DefaultOpen |
                            ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_SpanAvailWidth))
                        {
                            self(self, child);
                            ImGui::TreePop();
                        }
                    }
                };
                ImGui::PushID(filter.IsActive() ? "FilteredSettings" : "SettingsTree");
                drawGroup(drawGroup, root);
                ImGui::PopID();
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
        if (FrameBegun) ImGui::EndFrame();
        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        FrameBegun = true;
        DrawSettings();
    }

    void DrawWorldBox(const DirectX::BoundingBox& box, const DirectX::XMFLOAT4X4& viewProjection,
        const char* label, unsigned int color)
    {
        if (!FrameBegun || !ImGui::GetCurrentContext()) return;
        DirectX::XMFLOAT3 corners[8];
        box.GetCorners(corners);
        DirectX::XMFLOAT4 clip[8];
        const auto matrix = DirectX::XMLoadFloat4x4(&viewProjection);
        for (unsigned i = 0; i < 8; ++i)
            DirectX::XMStoreFloat4(&clip[i], DirectX::XMVector4Transform(
                DirectX::XMVectorSet(corners[i].x, corners[i].y, corners[i].z, 1), matrix));
        const auto size = ImGui::GetIO().DisplaySize;
        const auto screen = [size](const DirectX::XMFLOAT4& p)
        { return ImVec2((p.x / p.w + 1) * size.x * 0.5f, (1 - p.y / p.w) * size.y * 0.5f); };
        const auto distance = [](const DirectX::XMFLOAT4& p, unsigned plane)
        {
            switch (plane)
            {
            case 0: return p.w + p.x;
            case 1: return p.w - p.x;
            case 2: return p.w + p.y;
            case 3: return p.w - p.y;
            case 4: return p.z;
            default: return p.w - p.z;
            }
        };
        constexpr unsigned edges[12][2] = {{0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7}};
        auto* draw = ImGui::GetBackgroundDrawList();
        for (const auto& edge : edges)
        {
            auto a = clip[edge[0]], b = clip[edge[1]];
            bool visible = true;
            for (unsigned plane = 0; plane < 6; ++plane)
            {
                const float da = distance(a, plane), db = distance(b, plane);
                if (da < 0 && db < 0) { visible = false; break; }
                if (da < 0 || db < 0)
                {
                    const float t = da / (da - db);
                    const DirectX::XMFLOAT4 point(a.x + (b.x-a.x)*t, a.y + (b.y-a.y)*t,
                        a.z + (b.z-a.z)*t, a.w + (b.w-a.w)*t);
                    if (da < 0) a = point; else b = point;
                }
            }
            if (visible && a.w > 0.00001f && b.w > 0.00001f)
                draw->AddLine(screen(a), screen(b), color, 2);
        }
        const auto centre = DirectX::XMVector4Transform(
            DirectX::XMVectorSet(box.Center.x, box.Center.y, box.Center.z, 1), matrix);
        DirectX::XMFLOAT4 point;
        DirectX::XMStoreFloat4(&point, centre);
        bool visible = point.w > 0.00001f;
        for (unsigned plane = 0; plane < 6; ++plane) visible &= distance(point, plane) >= 0;
        if (visible) draw->AddText(screen(point), color, label);
    }

    void Render(GDX12CommandList* commands, GDX12Texture* target)
    {
        if (!ImGui::GetCurrentContext() || !FrameBegun) return;
        ImGui::Render();
        FrameBegun = false;
        if (!ImGui::GetDrawData()) return;
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
        FrameBegun = false;
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
