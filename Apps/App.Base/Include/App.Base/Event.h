#pragma once

#include <functional>
#include <vector>
#include <cstdint>
#include <algorithm>

using ListenerHandle = uint64_t;

template<typename... Args>
class Event
{
public:
    using Callback = std::function<void(Args...)>;

    ListenerHandle AddListener(Callback callback)
    {
        ListenerHandle handle = _nextHandle++;

        _listeners.push_back(Listener
        {
            handle,
            std::move(callback)
        });

        return handle;
    }

    void RemoveListener(ListenerHandle handle)
    {
        _listeners.erase(
            std::remove_if(
                _listeners.begin(),
                _listeners.end(),
                [handle](const Listener& listener)
                {
                    return listener.Handle == handle;
                }),
            _listeners.end()
        );
    }
    
    bool HasListener(ListenerHandle handle) const
    {
        return std::any_of(
            _listeners.begin(),
            _listeners.end(),
            [handle](const Listener& listener)
            {
                return listener.Handle == handle;
            });
    }

    void Broadcast(Args... args)
    {
        // listener can delete other listener during broadcast, tak chto pohui na main list
        auto listenersCopy = _listeners;

        for (auto& listener : listenersCopy)
        {
            if (HasListener(listener.Handle))
            {
                listener.Function(args...);
            }
        }
    }

    void Clear()
    {
        _listeners.clear();
    }

    bool IsEmpty() const
    {
        return _listeners.empty();
    }

private:
    struct Listener
    {
        ListenerHandle Handle;
        Callback Function;
    };
    
    ListenerHandle _nextHandle = 1;
    std::vector<Listener> _listeners;
};