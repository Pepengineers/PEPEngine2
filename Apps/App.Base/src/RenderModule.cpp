#include "App.Base/Modules/RenderModule.h"

#include "App.Base/Window.h"
#include "App.Base/App.h"
#include "Common/ConsoleVariables.h"


RenderModule::RenderModule(Window* window, GameTimer* timer) :
    _window(window), _timer(timer)
{
    _sceneRenderingSystem = std::make_unique<SceneRenderingSystem>(_window, _timer);
}

RenderModule::~RenderModule()
{
  
}

void RenderModule::Initialize()
{
    _sceneRenderingSystem->Initialize();
}

void RenderModule::Uninitialize()
{

}

void RenderModule::OnResize() const
{
    _sceneRenderingSystem->OnResize();
}

SceneRenderingSystem* RenderModule::GetSceneRenderer()
{
    return _sceneRenderingSystem.get();
}

void RenderModule::OnUpdate()
{
    _sceneRenderingSystem->Update();
}

void RenderModule::OnRender()
{
    _sceneRenderingSystem->Render();
}

bool RenderModule::ShouldTick()
{
    return true;
}

bool RenderModule::ShouldRender()
{
    return true;
}
