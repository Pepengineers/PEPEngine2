#include "Engine.Core/Engine.h"
#include "Engine.Core/ConsoleModule.h"

Engine* Engine::Instance = nullptr;

ModuleLocator& Engine::GetLocator()
{
    return Instance->Locator;
}

Engine* Engine::GetInstance()
{
    return Instance;
}

Engine::Engine()
{
    Instance = this;
}

bool Engine::Initialize()
{
    AddModules();
    for (auto& pair : Locator.registeredModules)
    {
        pair.second->Initialize();
    }

    return true;
}

void Engine::Update(const GameTimer& gameTimer)
{
    for (const auto& [type, module] : Locator.registeredModules)
    {
        module->Update();
    }
}

void Engine::Render(const GameTimer& gameTimer)
{
    for (const auto& [type, module] : Locator.registeredModules)
    {
        module->Render();
    }
}

bool Engine::AddModules()
{
    Locator.RegisterModule(std::make_shared<ConsoleModule>());
    return true;
}
