#pragma once

#include "Engine.Core/ECS/Entity.h"

class World;
class SceneManagerModule;

namespace Engine::UI
{
    class HierarchyPanel
    {
    public:
        void Draw(SceneManagerModule* sceneManager);
        void ApplyPendingCreate(SceneManagerModule* sceneManager);

    private:
        enum class CreateKind { None, Empty, Camera, Spline };
        CreateKind _pendingCreate = CreateKind::None;
        
        Entity _selectedEntity = InvalidEntity;
        World* _selectedWorld = nullptr;
    };
}
