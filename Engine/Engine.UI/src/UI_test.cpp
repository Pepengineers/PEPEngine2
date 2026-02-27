// UI_test.cpp

#include <Engine.UI/UI_test.h>

#include <imgui/imgui.h>
#include <imgui/backends/imgui_impl_dx12.h>
#include <imgui/backends/imgui_impl_win32.h>

namespace Engine::UI
{
	void Initialize ()
	{
		IMGUI_CHECKVERSION();
		ImGui::CreateContext();
		ImGuiIO& Io = ImGui::GetIO();
		Io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
		Io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;

		// Enable DPI awareness for fonts.
		Io.ConfigFlags |= ImGuiConfigFlags_DpiEnableScaleFonts;
		// Enable DPI awareness for windows/viewports.
		Io.ConfigFlags |= ImGuiConfigFlags_DpiEnableScaleViewports;

		ImGui::StyleColorsDark();

		ImGuiStyle& Style = ImGui::GetStyle();
		if (Io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
		{
			Style.WindowRounding = 0.0f;
			Style.Colors[ImGuiCol_WindowBg].w = 1.0f;
		}
	}

	void ShutdownUI ()
	{
		ImGui::DestroyContext();
	}
}