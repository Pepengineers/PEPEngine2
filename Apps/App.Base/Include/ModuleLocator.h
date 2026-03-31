#pragma once

#include <cassert>
#include <functional>
#include <memory>
#include <type_traits>
#include <typeindex>
#include <typeinfo>
#include <unordered_map>

#include "AppModule.h"

class App;


class ModuleLocator final
{
public:
    using ModulePtr = std::shared_ptr<AppModule>;
    using Factory = std::function<ModulePtr()>;

    template <typename T, typename = std::enable_if_t<std::is_base_of_v<AppModule, T>>>
    std::shared_ptr<T> GetModule()
    {
        const auto typeKey = std::type_index(typeid(T));

        const auto registeredIt = registeredModules.find(typeKey);
        if (registeredIt != registeredModules.end() && registeredIt->second)
        {
            return std::dynamic_pointer_cast<T>(registeredIt->second);
        }

        const auto factoryIt = factories.find(typeKey);
        if (factoryIt == factories.end() || !factoryIt->second)
        {
            return nullptr;
        }

        ModulePtr service = factoryIt->second();
        if (!service)
        {
            return nullptr;
        }

        registeredModules[typeKey] = std::move(service);
        return std::dynamic_pointer_cast<T>(registeredModules[typeKey]);
    }

    template <typename T, typename = std::enable_if_t<std::is_base_of_v<AppModule, T>>>
    ModuleLocator& GetModule(std::shared_ptr<T>& value)
    {
        value = GetModule<T>();
        return *this;
    }

    template <typename T, typename = std::enable_if_t<std::is_base_of_v<AppModule, T>>>
    ModuleLocator& BindModule(Factory resolver)
    {
        assert(static_cast<bool>(resolver));
        const auto typeKey = std::type_index(typeid(T));

        factories[typeKey] = std::move(resolver);
        return *this;
    }

    template <typename T, typename = std::enable_if_t<std::is_base_of_v<AppModule, T>>>
    void UnbindModule()
    {
        const auto typeKey = std::type_index(typeid(T));
        factories.erase(typeKey);
        RemoveRegisteredType(typeKey);
    }

    template <typename T, typename = std::enable_if_t<std::is_base_of_v<AppModule, T>>>
    ModuleLocator& RegisterModule(std::shared_ptr<T> subsystem)
    {
        assert(subsystem != nullptr);
        const auto typeKey = std::type_index(typeid(T));

        const auto existingIt = registeredModules.find(typeKey);
        if (existingIt != registeredModules.end())
        {
            TryUninitialize(existingIt->second);
        }
        registeredModules[typeKey] = std::move(subsystem);

        return *this;
    }

    template <typename T, typename = std::enable_if_t<std::is_base_of_v<AppModule, T>>>
    void UnregisterModule()
    {
        const auto typeKey = std::type_index(typeid(T));
        RemoveRegisteredType(typeKey);
    }

private:
    friend class App;

    void Clear()
    {
        for (auto& pair : registeredModules)
        {
            TryUninitialize(pair.second);
        }

        registeredModules.clear();
        factories.clear();
    }

    static void TryUninitialize(const ModulePtr& module)
    {
        if (!module)
        {
            return;
        }

        try
        {
            module->Uninitialize();
        }
        catch (...)
        {
        }
    }

    void RemoveRegisteredType(const std::type_index& type)
    {
        const auto it = registeredModules.find(type);
        if (it == registeredModules.end())
        {
            return;
        }

        TryUninitialize(it->second);
        registeredModules.erase(it);
    }

    std::unordered_map<std::type_index, ModulePtr> registeredModules;
    std::unordered_map<std::type_index, Factory> factories;
};
