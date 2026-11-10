#include "Engine.UI/Editor/HierarchyPanel.h"

#include <string>

#include "App.Base/Modules/SceneManagerModule.h"
#include "imgui/imgui.h"

namespace Engine::UI
{
    void HierarchyPanel::Draw(SceneManagerModule* sceneManager)
    {
        if (!ImGui::Begin("Hierarchy"))
        {
            ImGui::End();
            return;
        }

        if (!sceneManager || sceneManager->GetWorldCount() == 0)
        {
            _selectedWorld = nullptr;
            _selectedEntity = InvalidEntity;
            ImGui::TextDisabled("No world loaded");
        }

        for (size_t i = 0; sceneManager && i < sceneManager->GetWorldCount(); ++i)
        {
            World* world = sceneManager->GetWorld(i);
            auto& ecs = world->GetECS();
            if (_selectedWorld == world && !ecs.IsAlive(_selectedEntity))
            {
                _selectedEntity = InvalidEntity;
            }

            ImGui::PushID(static_cast<int>(i));
            const std::string worldName = world->GetPath().stem().string();
            if (ImGui::TreeNodeEx("World", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth,
                                  "%s", worldName.c_str()))
            {
                if (ecs.GetEntityCount() == 0)
                {
                    ImGui::TextDisabled("World is empty");
                }

                ecs.ForEachEntity([&](Entity entity)
                {
                    const std::string name = ecs.Has<NameComponent>(entity)
                        ? ecs.Get<NameComponent>(entity).value : "Entity " + std::to_string(entity);
                    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Leaf
                        | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth;
                    if (_selectedWorld == world && _selectedEntity == entity)
                    {
                        flags |= ImGuiTreeNodeFlags_Selected;
                    }

                    ImGui::PushID(static_cast<int>(entity));
                    ImGui::TreeNodeEx("Entity", flags, "%s", name.c_str());
                    if (ImGui::IsItemClicked())
                    {
                        _selectedWorld = world;
                        _selectedEntity = entity;
                    }
                    ImGui::PopID();
                });
                ImGui::TreePop();
            }
            ImGui::PopID();
        }

        ImGui::End();
    }
}
