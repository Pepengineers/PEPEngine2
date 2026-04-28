#pragma once
#include "ModuleLocator.h"
#include "Common/GameTimer.h"

class Engine
{
public:
    static ModuleLocator& GetLocator();
    static Engine* GetInstance();

    Engine();
    virtual ~Engine() = default;
    virtual bool Initialize();

    virtual void Update(const GameTimer& gameTimer);
    virtual void Render(const GameTimer& gameTimer);

protected:
    virtual bool AddModules();

    GameTimer Timer;
    ModuleLocator Locator;
    static Engine* Instance;
};
