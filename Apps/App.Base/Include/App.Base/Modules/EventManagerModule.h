#pragma once

#include "Common/GameTimer.h"
#include "Common/Module.h"
#include "Common/ConsoleVariables.h"

#include "App.Base/Event.h"

class EventManagerModule final : public Module
{
public:
    EventManagerModule(GameTimer* timer);
    ~EventManagerModule() override;

    void Initialize() override;
    void Uninitialize() override;

    template<typename TEvent>
    ListenerHandle AddListener(std::function<void(const TEvent&)> listener);
    
    template<typename TEvent>
    void RemoveListener(ListenerHandle listenerHandle);
    
    template<typename TEvent>
    void Broadcast(const TEvent& event);
    
protected:
    void OnUpdate() override;
    void OnRender() override;

    bool ShouldTick() override;
    bool ShouldRender() override;
};
