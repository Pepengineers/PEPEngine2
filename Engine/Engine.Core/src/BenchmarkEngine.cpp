#include "Engine.Core/BenchmarkEngine.h"
#include "Engine.Core/ConsoleModule.h"
#include "Engine.Core/Logging/Logger.h"

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
    auto console = std::make_shared<ConsoleModule>();
    console->Initialize();
    Locator.RegisterModule(std::move(console));
    Log::Info("Engine", "Core configuration and console module ready");
    Instance = this;
}

bool BenchmarkEngine::Initialize()
{
    if (!AddModules()) return false;
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
    return true;
}
