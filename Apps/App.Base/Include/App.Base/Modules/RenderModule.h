#pragma once

#include "App.Base/App.h"

#include "App.Base/Systems/SceneRenderingSystem.h"

class RenderModule final : public Module
{
public:
    RenderModule(Window* window, GameTimer* timer);
    ~RenderModule() override;

    void Initialize() override;
    void Uninitialize() override;

    void OnResize() const;

    SceneRenderingSystem* GetSceneRenderer();

protected:
    void OnUpdate() override;
    void OnRender() override;

    bool ShouldTick() override;
    bool ShouldRender() override;

private:
    GameTimer* _timer;
    Window* _window;

    std::unique_ptr<SceneRenderingSystem> _sceneRenderingSystem;
};
