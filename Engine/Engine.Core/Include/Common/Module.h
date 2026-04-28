#pragma once
#include "Stopwatch.h"

class Module
{
public:
    virtual ~Module() = default;
    virtual void Initialize() = 0;
    virtual void Uninitialize() = 0;

    void Update()
    {
        if (ShouldTick())
        {
            Stopwatch stopwatch;
            OnUpdate();
            //TODO: Log it
            stopwatch.GetElapsed();
        }
    }

    void Render()
    {
        if (ShouldRender())
        {
            Stopwatch stopwatch;
            OnRender();
            //TODO: Log it
            stopwatch.GetElapsed();
        }
    }

protected:
    virtual bool ShouldTick() { return false; }

    virtual bool ShouldRender() { return false; };

    virtual void OnUpdate()
    {
    }

    virtual void OnRender()
    {
    };
};
