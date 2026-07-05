#pragma once

#include "Engine.Core/ECS/Entity.h"

class World;

namespace Engine::UI
{
    class HierarchyPanel
    {
    public:
        void Draw(World* world);

        Entity GetSelectedEntity();
        void SetSelectedEntity(Entity entity);

    private:
        Entity _selectedEntity = InvalidEntity;
    };
}
