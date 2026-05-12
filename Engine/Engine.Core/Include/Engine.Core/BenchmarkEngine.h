#pragma once
#include "ModuleLocator.h"
#include "Common/GameTimer.h"

class BenchmarkEngine
{
public:
    static ModuleLocator& GetLocator();
    static BenchmarkEngine* GetInstance();

    BenchmarkEngine();
    virtual ~BenchmarkEngine() = default;
    virtual bool Initialize();

    virtual void Update(const GameTimer& gameTimer);
    virtual void Render(const GameTimer& gameTimer);

protected:
    virtual bool AddModules();

    GameTimer Timer;
    ModuleLocator Locator;
    static BenchmarkEngine* Instance;
};
