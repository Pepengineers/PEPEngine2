#include "Engine.UI/Editor/HierarchyPanel.h"

#include <string>

#include "App.Base/ECS/Components/NameComponent.h"
#include "App.Base/ECS/World.h"
#include "imgui/imgui.h"

namespace Engine::UI
{
    void HierarchyPanel::Draw(World* world)
    {
        ImGui::Begin("Hierarchy");

        if (!world)
        {
            _selectedEntity = InvalidEntity;
            ImGui::TextDisabled("No world loaded");
            ImGui::End();
            return;
        }

        auto& ecs = world->GetECS();
        if (_selectedEntity != InvalidEntity && !ecs.IsAlive(_selectedEntity))
        {
            _selectedEntity = InvalidEntity;
        }

        bool hasNamedEntities = false;

        ecs.ForEach<NameComponent>(
            [this, &hasNamedEntities](Entity entity, NameComponent& nameComponent)
            {
                hasNamedEntities = true;

                const std::string label = nameComponent.value + "##" + std::to_string(entity);
                if (ImGui::Selectable(label.c_str(), _selectedEntity == entity))
                {
                    _selectedEntity = entity;
                }
            });

        if (!hasNamedEntities)
        {
            ImGui::TextDisabled("World has no named entities");
        }

        ImGui::End();
    }

    Entity HierarchyPanel::GetSelectedEntity()
    {
        return _selectedEntity;
    }

    void HierarchyPanel::SetSelectedEntity(Entity entity)
    {
        _selectedEntity = entity;
    }
}
