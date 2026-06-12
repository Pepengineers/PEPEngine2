#pragma once

class World;

namespace Engine::UI
{
    class HierarchyPanel
    {
    public:
        void Draw(World* world);

        //TODO uncomment later
        //Entity GetSelectedEntity();
        //void SetSelectedEntity(Entity entity);

    private:
        //Entity _selectedEntity;
    };
}
