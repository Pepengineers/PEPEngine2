#include "Engine.UI/Editor/HierarchyPanel.h"

#include <string>

#include "imgui_internal.h"
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
        
        const bool cutShortcut = ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_X);
        const bool pasteShortcut = ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_V);
        
        World* pasteWorld = nullptr;
        Entity pasteParent = InvalidEntity;
        bool cutSourceAlive = false;

        for (size_t i = 0; sceneManager && i < sceneManager->GetWorldCount(); ++i)
        {
            World* world = sceneManager->GetWorld(i);
            auto& ecs = world->GetECS();
            
            if (_selectedWorld == world && _selectedEntity != InvalidEntity && !ecs.IsAlive(_selectedEntity))
            {
                _selectedWorld = nullptr;
                _selectedEntity = InvalidEntity;
            }

            const auto markCut = [&](Entity entity)
            {
                _cutWorld = world;
                _cutEntity = entity;
            };
            
            const auto canPaste = [&](Entity parent)
            {
                return _cutWorld == world && world->CanSetParent(_cutEntity, parent);
            };
            
            const auto queuePaste = [&](Entity parent)
            {
                pasteWorld = world;
                pasteParent = parent;
            };
            
            const auto handleRow = [&](Entity entity)
            {
                if (ImGui::IsItemClicked() || ImGui::IsItemClicked(ImGuiMouseButton_Right))
                {
                    _selectedWorld = world;
                    _selectedEntity = entity;
                }
                    
                if (ImGui::BeginPopupContextItem())
                {
                    if (entity != InvalidEntity && ImGui::MenuItem("Cut", "Ctrl+X"))
                    {
                        markCut(entity);
                    }
                    
                    const char* label = entity == InvalidEntity ? "Paste at Root" : "Paste as Child";
                    
                    if (ImGui::MenuItem(label, "Ctrl+V", false, canPaste(entity)))
                    {
                        queuePaste(entity);
                    }
                    
                    ImGui::EndPopup();
                }
            };
            
            std::unordered_map<Entity, std::vector<Entity>> children;
            ecs.ForEachEntity([&](Entity entity)
            {
                children[world->GetParent(entity)].push_back(entity);
            });
            
            ImGui::PushID(static_cast<int>(i));
            
            const auto drawEntity = [&](const auto& self, Entity entity) -> void
            {
                const auto childList = children.find(entity);
                const bool hasChildren = childList != children.end();
                
                const std::string name = ecs.Has<NameComponent>(entity) ? ecs.Get<NameComponent>(entity).value : "Entity " + std::to_string(entity);
                
                ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth;
                if (hasChildren)
                {
                    flags |= ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen;
                }
                else
                {
                    flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
                }
                
                if (_selectedWorld == world && _selectedEntity == entity)
                {
                    flags |= ImGuiTreeNodeFlags_Selected;
                }
                
                ImGui::PushID(static_cast<int>(entity));
                    
                const bool isCut = _cutWorld == world && _cutEntity == entity;
                if (isCut)
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                }
                
                const bool open = ImGui::TreeNodeEx("Entity", flags, "%s", name.c_str());
                
                if (isCut)
                {
                    ImGui::PopStyleColor();
                }
                
                handleRow(entity);
                
                if (open && hasChildren)
                {
                    for (Entity child : childList->second)
                    {
                        self(self, child);
                    }
                    
                    ImGui::TreePop();
                }
                    
                ImGui::PopID();
            };
            
            ImGuiTreeNodeFlags worldFlags = ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
            if (_selectedWorld == world && _selectedEntity == InvalidEntity)
            {
                worldFlags |= ImGuiTreeNodeFlags_Selected;
            }
            
            const std::string worldName = world->GetPath().stem().string();
            const bool worldOpen = ImGui::TreeNodeEx("World", worldFlags, "%s", worldName.c_str());
            
            handleRow(InvalidEntity);
            
            if (worldOpen)
            {
                const auto roots = children.find(InvalidEntity);
                if (roots == children.end())
                {
                    ImGui::TextDisabled("World is empty");
                }
                else
                {
                    for (Entity entity : roots->second)
                    {
                        drawEntity(drawEntity, entity);
                    }
                }
                
                ImGui::TreePop();
            }
            
            ImGui::PopID();
            
            if (_selectedWorld == world)
            {
                if (cutShortcut && ecs.IsAlive(_selectedEntity))
                {
                    markCut(_selectedEntity);
                }
                
                if (pasteShortcut && canPaste(_selectedEntity))
                {
                    queuePaste(_selectedEntity);
                }
            }
            
            cutSourceAlive |= _cutWorld == world && ecs.IsAlive(_cutEntity);
        }
        
        if (pasteWorld && pasteWorld == _cutWorld && pasteWorld->SetParent(_cutEntity, pasteParent))
        {
            _selectedWorld = pasteWorld;
            _selectedEntity = _cutEntity;
            cutSourceAlive = false;
        }
        
        if (!cutSourceAlive)
        {
            _cutWorld = nullptr;
            _cutEntity = InvalidEntity;
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
