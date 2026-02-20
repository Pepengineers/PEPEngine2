// UI_test.cpp

#include <Engine.UI/UI_test.h>

#include <imgui/imgui.h>
#include <imgui/backends/imgui_impl_dx12.h>
#include <imgui/backends/imgui_impl_win32.h>

namespace Engine::UI
{
    void Initialize()
    {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;

        // Enable DPI awareness for fonts
        io.ConfigFlags |= ImGuiConfigFlags_DpiEnableScaleFonts;
        // Enable DPI awareness for windows/viewports
        io.ConfigFlags |= ImGuiConfigFlags_DpiEnableScaleViewports;


        ImGui::StyleColorsDark();

        ImGuiStyle& style = ImGui::GetStyle();
        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
            style.WindowRounding = 0.0f;
            style.Colors[ImGuiCol_WindowBg].w = 1.0f;
        }
    }

    void ShutdownUI()
    {
        ImGui::DestroyContext();
    }
}