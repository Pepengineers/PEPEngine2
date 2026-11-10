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

    private:
        Entity _selectedEntity = InvalidEntity;
        World* _selectedWorld = nullptr;
    };
}
