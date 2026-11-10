#include "Engine.UI/Editor/HierarchyPanel.h"

#include <string>

#include "App.Base/Modules/RenderModule.h"
#include "App.Base/Modules/SceneManagerModule.h"
#include "Engine.Core/BenchmarkEngine.h"
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

        ImGui::BeginDisabled(!sceneManager || sceneManager->GetWorldCount() == 0);

        if (ImGui::Button("+"))
        {
            ImGui::OpenPopup("CreateObject");
        }

        if (ImGui::BeginPopup("CreateObject"))
        {
            if (ImGui::MenuItem("Empty Object"))
            {
                _pendingCreate = CreateKind::Empty;
            }

            if (ImGui::MenuItem("Camera"))
            {
                _pendingCreate = CreateKind::Camera;
            }

            if (ImGui::MenuItem("Spline"))
            {
                _pendingCreate = CreateKind::Spline;
            }

            ImGui::Separator();
            ImGui::MenuItem("Mesh (asset required)", nullptr, false, false);

            ImGui::EndPopup();
        }

        ImGui::EndDisabled();
        ImGui::Separator();

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
                                                 ? ecs.Get<NameComponent>(entity).value
                                                 : "Entity " + std::to_string(entity);
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

    void HierarchyPanel::ApplyPendingCreate(SceneManagerModule* sceneManager)
    {
        const CreateKind kind = _pendingCreate;
        _pendingCreate = CreateKind::None;

        if (kind == CreateKind::None)
        {
            return;
        }

        World* world = sceneManager ? sceneManager->GetWorld() : nullptr;
        if (!world)
        {
            return;
        }

        if (kind != CreateKind::Spline)
        {
            auto renderer = BenchmarkEngine::GetLocator().GetModule<RenderModule>();
            renderer->GetPrimaryDevice()->GetCommandQueue()->Flush();
        }

        auto& ecs = world->GetECS();
        auto entity = ecs.CreateEntity();

        const char* baseName = kind == CreateKind::Camera
                                   ? "Camera"
                                   : kind == CreateKind::Spline
                                   ? "Spline"
                                   : "Empty";

        entity.AddComponent<NameComponent>(std::string(baseName) + " " + std::to_string(entity.GetId()));

        if (kind == CreateKind::Spline)
        {
            entity.AddComponent<SplineCurveComponent>(false, std::vector<SplinePoint>{
                                                          {Vector3(-50.f, 0.f, 0.f), Vector3::Zero, Vector3::Zero},
                                                          {Vector3(50.f, 0.f, 0.f), Vector3::Zero, Vector3::Zero}
                                                      });
        }
        else
        {
            entity.AddComponent<TransformComponent>();

            if (kind == CreateKind::Camera)
            {
                entity.AddComponent<CameraComponent>();
            }

            //reuploading existing contents because resizing replaces gpu buffers
            for (size_t i = 0; i < sceneManager->GetWorldCount(); ++i)
            {
                auto& worldEcs = sceneManager->GetWorld(i)->GetECS();

                worldEcs.ForEach<TransformComponent>([](Entity, TransformComponent& transform)
                {
                    transform.DirtyFlag = true;
                });
                
                worldEcs.ForEach<CameraComponent>([](Entity, CameraComponent& camera)
                {
                    camera.DirtyFlag = true;
                });
            }
        }
        
        _selectedWorld = world;
        _selectedEntity = entity.GetId();
    }
}
