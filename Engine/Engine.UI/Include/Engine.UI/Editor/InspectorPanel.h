#pragma once
#include <cstdint>

#include "Engine.Core/ECS/Entity.h"

class World;

namespace Engine::UI
{
    enum class GizmoOperation : uint8_t
    {
        Translate,
        Rotate,
        Scale
    };
    
    class InspectorPanel
    {
    public:
        void DrawInspectorPanel(World* world, Entity selectedEntity);

        void DrawGizmo(World* world,
                        Entity selectedEntity,
                        const float* viewMatrix,
                        const float* projectionMatrix,
                        float viewportX,
                        float viewportY,
                        float viewportWidth,
                        float viewportHeight);

        GizmoOperation GetGizmoOperation();
        void SetGizmoOperation(GizmoOperation operation);

        bool IsUsingGizmo();

    private:
        GizmoOperation _gizmoOperation = GizmoOperation::Translate;
        bool _isUsingGizmo = false;
    };

}
