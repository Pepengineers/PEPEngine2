#pragma once

class Module
{
public:
    virtual ~Module() = default;
    virtual void Initialize() = 0;
    virtual void Uninitialize() = 0;

    void Update()
    {
        if (ShouldTick()) OnUpdate();
    }

    void Render()
    {
        if (ShouldRender()) OnRender();
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
