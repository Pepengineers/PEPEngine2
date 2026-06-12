#include <Engine.UI/Editor/EditorPanels.h>
#include <App.Base/Entity.h>
#include "App.Base/Modules/SceneManagerModule.h"
#include "Engine.UI/EditorUI.h"
#include "imgui/imgui.h"

namespace Engine::UI
{
    void EditorPanels::DrawAll(SceneManagerModule* sceneManager, const float* viewMatrix, const float* projectionMatrix, float viewportX, float viewportY, float viewportWidth, float viewportHeight)
    {
        DrawToolbar();

        World* world = sceneManager ? sceneManager->GetWorld(_currentWorldIndex) : nullptr;

        _hierarchyPanel.Draw(world);
        _inspectorPanel.DrawInspectorPanel(world, _hierarchyPanel.GetSelectedEntity());

        if (GetEditorMode() == EditorMode::Edit)
        {
            _inspectorPanel.DrawGizmo(world,
                                        _hierarchyPanel.GetSelectedEntity(),
                                        viewMatrix,
                                        projectionMatrix,
                                        viewportX,
                                        viewportY,
                                        viewportWidth,
                                        viewportHeight);
        }

        _assetBrowserPanel.Draw(ASSETS_FOLDER);
    }

    Entity EditorPanels::GetSelectedEntity()
    {
        return _hierarchyPanel.GetSelectedEntity();
    }

    bool EditorPanels::IsGizmoEnabled()
    {
        return _inspectorPanel.IsUsingGizmo();
    }

    void EditorPanels::DrawToolbar()
    {
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse;
        ImGui::Begin("Toolbar", nullptr, flags);

        EditorMode mode = GetEditorMode();

        if (mode == EditorMode::Edit)
        {
            if (ImGui::Button("Play"))
            {
                SetEditorMode(EditorMode::Play);
            }
        }
        else
        {
            if (ImGui::Button("Stop"))
            {
                SetEditorMode(EditorMode::Edit);
            }
        }

        ImGui::SameLine();
        ImGui::TextDisabled(mode == EditorMode::Play ? "(Play Mode - simulation running)" : "(Edit Mode)");

        ImGui::End();
    }


}
