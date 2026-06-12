#pragma once
#include "AssetBrowserPanel.h"
#include "HierarchyPanel.h"
#include "InspectorPanel.h"

class SceneManagerModule;

namespace Engine::UI
{
    class EditorPanels
    {
    public:
        void DrawAll(SceneManagerModule* sceneManager,
                    const float* viewMatrix,
                    const float* projectionMatrix,
                    float viewportX,
                    float viewportY,
                    float viewportWidth,
                    float viewportHeight);

        Entity GetSelectedEntity();
        bool IsGizmoEnabled();

    private:
        void DrawToolbar();

        HierarchyPanel _hierarchyPanel;
        InspectorPanel _inspectorPanel;
        AssetBrowserPanel _assetBrowserPanel;

        int _currentWorldIndex = 0;
    };
}


