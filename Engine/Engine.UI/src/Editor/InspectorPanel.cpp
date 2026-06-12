#include "Engine.UI/Editor/InspectorPanel.h"

#include "imgui/imgui.h"

struct NameComponent;

namespace Engine::UI
{
    GizmoOperation InspectorPanel::GetGizmoOperation()
    {
        return _gizmoOperation;
    }

    void InspectorPanel::SetGizmoOperation(GizmoOperation operation)
    {
        _gizmoOperation = operation;
    }

    bool InspectorPanel::IsUsingGizmo()
    {
        return _isUsingGizmo;
    }

    void InspectorPanel::DrawInspectorPanel(World* world, Entity selectedEntity)
    {
        ImGui::Begin("Inspector");

        if (!world || selectedEntity == InvalidEntity || !world->GetECS().IsAlive(selectedEntity))
        {
            ImGui::TextDisabled("No object selected");
            ImGui::End();
            return;
        }

        auto& ecs = world->GetECS();

        if (ecs.Has<NameComponent>(selectedEntity))
        {
            NameComponent& nameComponent = ecs.Get<NameComponent>(selectedEntity);

            char buffer[256];
            strncpy_s(buffer, nameComponent.value.c_str(), sizeof(buffer) - 1);

            if (ImGui::InputText("Name", buffer, sizeof(buffer)))
            {
                nameComponent.value = buffer;
            }
        }

        ImGui::Separator();

        if (ecs.Has<TransformComponent>(selectedEntity))
        {
            TransformComponent& transform = ecs.Get<TransformComponent>(selectedEntity);

            bool changed = false;
            changed |= ImGui::DragFloat3("Location", &transform.Location.x, 0.1f);
            changed |= ImGui::DragFloat3("Rotation", &transform.Rotation.x, 0.5f);
            changed |= ImGui::DragFloat3("Scale", &transform.Scale.x, 0.05f, 0.0001f, 100000.0f);

            if (changed)
            {
                transform.DirtyFlag = true;
                ecs.MarkComponentUpdated<TransformComponent>(selectedEntity);
            }
        }

        ImGui::Separator();

        ImGui::Text("Gizmo");
        if (ImGui::RadioButton("Translate", _gizmoOperation == GizmoOperation::Translate))
        {
            _gizmoOperation = GizmoOperation::Translate;
        }
        if (ImGui::RadioButton("Scale", _gizmoOperation == GizmoOperation::Scale))
        {
            _gizmoOperation = GizmoOperation::Scale;
        }
        if (ImGui::RadioButton("Rotate", _gizmoOperation == GizmoOperation::Rotate))
        {
            _gizmoOperation = GizmoOperation::Rotate;
        }

        //TODO: the fuck is that
        if (ecs.Has<StaticMeshRenderComponent>(selectedEntity))
        {
            ImGui::Separator();
            ImGui::TextDisabled("Static Mesh Render Component attached");
        }

        ImGui::End();
    }

    void InspectorPanel::DrawGizmo(World* world, __resharper_unknown_type selectedEntity, const float* viewMatrix, const float* projectionMatrix, float viewportX, float viewportY, float viewportWidth, float viewportHeight)
    {
        _isUsingGizmo = false;

        if (!world || selectedEntity == InvalidEntity || !world->GetECS().IsAlive(selectedEntity))
        {
            return;
        }

        auto& ecs = world->GetECS();
        if (!ecs.Has<TransformComponent>(selectedEntity))
        {
            return;
        }

        TransformComponent& transformComponent = ecs.Get<TransformComponent>(selectedEntity);

        ImGuizmo::SetOrthographic(false);
        ImGuizmo::SetDrawlist();
        ImGuizmo::SetRect(viewportX, viewportY, viewportWidth, viewportHeight);

        Matrix worldMatrix =
                    Matrix::CreateScale(transformComponent.Scale) *
                    Matrix::CreateFromYawPitchRoll(
                        DirectX::XMConvertToRadians(transformComponent.Rotation.y),
                        DirectX::XMConvertToRadians(transformComponent.Rotation.x),
                        DirectX::XMConvertToRadians(transformComponent.Rotation.z)
                        ) *
                    Matrix::CreateTranslation(transformComponent.Location);

        ImGuizmo::OPERATION operation = ImGuizmo::TRANSLATE;

        switch (_gizmoOperation)
        {
            case GizmoOperation::Translate:
                operation = ImGuizmo::TRANSLATE;
                break;
            case GizmoOperation::Scale:
                operation = ImGuizmo::SCALE;
                break;
            case GizmoOperation::Rotate:
                operation = ImGuizmo::ROTATE;
                break;
        }

        const bool manipulated = ImGuizmo::Manipulate(viewMatrix,
                                                        projectionMatrix,
                                                        operation,
                                                        ImGuizmo::WORLD,
                                                        &worldMatrix);

        _isUsingGizmo = ImGuizmo::IsUsing();

        if (manipulated)
        {
            float translation[3], rotation[3], scale[3];
            ImGuizmo::DecomposeMatrixToComponents(&worldMatrix, translation, rotation, scale);

            transformComponent.Location = Vector3(translation[0], translation[1], translation[2]);
            transformComponent.Rotation = Vector3(rotation[0], rotation[1], rotation[2]);
            transformComponent.Scale = Vector3(scale[0], scale[1], scale[2]);
            transformComponent.DirtyFlag = true;

            ecs.MarkComponentUpdated<TransformComponent>(selectedEntity);
        }
    }

}
