#include "Engine.Core/BenchmarkEngine.h"
#include "Engine.Core/ConsoleModule.h"

BenchmarkEngine* BenchmarkEngine::Instance = nullptr;

ModuleLocator& BenchmarkEngine::GetLocator()
{
    return Instance->Locator;
}

BenchmarkEngine* BenchmarkEngine::GetInstance()
{
    return Instance;
}

BenchmarkEngine::BenchmarkEngine()
{
    Instance = this;
}

bool BenchmarkEngine::Initialize()
{
    AddModules();
    for (auto& pair : Locator.registeredModules)
    {
        pair.second->Initialize();
    }

    return true;
}

void BenchmarkEngine::Update(const GameTimer& gameTimer)
{
    for (const auto& [type, module] : Locator.registeredModules)
    {
        module->Update();
    }
}

void BenchmarkEngine::Render(const GameTimer& gameTimer)
{
    for (const auto& [type, module] : Locator.registeredModules)
    {
        module->Render();
    }
}

bool BenchmarkEngine::AddModules()
{
    Locator.RegisterModule(std::make_shared<ConsoleModule>());
    return true;
}
