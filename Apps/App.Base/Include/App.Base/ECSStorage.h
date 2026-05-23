#pragma once
#include <tuple>
#include <vector>
#include <limits>
#include <cstdint>
#include <type_traits>
#include <utility>
#include <App.Base/Entity.h>
#include <App.Base/Components/Component.h>
#include <App.Base/Event.h>

#undef max;

static constexpr Entity InvalidEntity = 0;
static constexpr size_t InvalidIndex = std::numeric_limits<size_t>::max();

template<typename T, typename... Ts>
struct IsOneOf : std::false_type
{
};

template<typename T, typename First, typename... Rest>
struct IsOneOf<T, First, Rest...>
    : std::conditional<std::is_same<T, First>::value, std::true_type, IsOneOf<T, Rest...>>::type
{
};

template<typename T>
class ComponentPool
{
public:
    Event<Entity, T&> OnComponentCreated;
    Event<Entity, T&> OnComponentDestroyed;
    Event<Entity, T&> OnComponentUpdated;
    
    void Clear()
    {
        for (size_t i = 0; i < _denseComponents.size(); ++i)
        {
            OnComponentDestroyed.Broadcast(_denseEntities[i], _denseComponents[i]);
        }

        _sparse.clear();
        _denseEntities.clear();
        _denseComponents.clear();
    }

    bool Has(Entity entity) const
    {
        if (entity >= _sparse.size()) { return false; }

        size_t denseIndex = _sparse[entity];
        if (denseIndex == InvalidIndex) { return false; }

        return denseIndex < _denseEntities.size() && _denseEntities[denseIndex] == entity;
    }

    T& Add(Entity entity, const T& value = T{})
    {
        if (entity >= _sparse.size()) { _sparse.resize(static_cast<size_t>(entity) + 1, InvalidIndex); }

        if (Has(entity))
        {
            T& existingComponent = _denseComponents[_sparse[entity]];
            existingComponent = value;

            OnComponentUpdated.Broadcast(entity, existingComponent);

            return existingComponent;
        }

        size_t newIndex = _denseComponents.size();
        _sparse[entity] = newIndex;
        _denseEntities.push_back(entity);
        _denseComponents.push_back(value);

        T& createdComponent = _denseComponents.back();

        OnComponentCreated.Broadcast(entity, createdComponent);

        return createdComponent;
    }

    template<typename... Args>
    T& Emplace(Entity entity, Args&&... args)
    {
        if (entity >= _sparse.size())
        {
            _sparse.resize(static_cast<size_t>(entity) + 1, InvalidIndex);
        }

        if (Has(entity))
        {
            T& existingComponent = _denseComponents[_sparse[entity]];
            existingComponent = T(std::forward<Args>(args)...);

            OnComponentUpdated.Broadcast(entity, existingComponent);

            return existingComponent;
        }

        size_t newIndex = _denseComponents.size();
        _sparse[entity] = newIndex;
        _denseEntities.push_back(entity);
        _denseComponents.emplace_back(std::forward<Args>(args)...);

        T& createdComponent = _denseComponents.back();

        OnComponentCreated.Broadcast(entity, createdComponent);

        return createdComponent;
    }

    void Remove(Entity entity)
    {
        if (!Has(entity)) { return; }

        size_t removeIndex = _sparse[entity];
        
        OnComponentDestroyed.Broadcast(entity, _denseComponents[removeIndex]);

        size_t lastIndex = _denseComponents.size() - 1;
        Entity lastEntity = _denseEntities[lastIndex];

        if (removeIndex != lastIndex)
        {
            _denseComponents[removeIndex] = std::move(_denseComponents[lastIndex]);
            _denseEntities[removeIndex] = lastEntity;
            _sparse[lastEntity] = removeIndex;
        }

        _denseComponents.pop_back();
        _denseEntities.pop_back();
        _sparse[entity] = InvalidIndex;
    }

    void MarkComponentUpdated(Entity entity)
    {
        if (!Has(entity)) { return; }

        T& component = Get(entity);
        OnComponentUpdated.Broadcast(entity, component);
    }

    T& Get(Entity entity)
    {
        return _denseComponents[_sparse[entity]];
    }

    const T& Get(Entity entity) const
    {
        return _denseComponents[_sparse[entity]];
    }

    size_t Size() const
    {
        return _denseComponents.size();
    }

    const std::vector<Entity>& GetEntities() const
    {
        return _denseEntities;
    }

    std::vector<T>& GetDense()
    {
        return _denseComponents;
    }

    const std::vector<T>& GetDense() const
    {
        return _denseComponents;
    }

private:
    std::vector<size_t> _sparse;
    std::vector<Entity> _denseEntities;
    std::vector<T> _denseComponents;
};

template<typename... Components>
class ECSStorage
{
public:
    class EntityHandle 
    {
    public:
        EntityHandle() = default;

        Entity GetId() const
        {
            return _entity;
        }

        explicit operator bool() const
        {
            return _storage != nullptr && _storage->IsAlive(_entity);
        }

        operator Entity() const
        {
            return _entity;
        }

        template<typename T, typename... Args>
        T& AddComponent(Args&&... args)
        {
            return _storage->template Add<T>(_entity, std::forward<Args>(args)...);
        }

        template<typename T>
        void RemoveComponent()
        {
            _storage->Remove<T>(_entity);
        }

        template<typename T>
        bool HasComponent() const
        {
            return _storage->Has<T>(_entity);
        }

        template<typename T>
        T& GetComponent()
        {
            return _storage->Get<T>(_entity);
        }

        template<typename T>
        const T& GetComponent() const
        {
            return static_cast<const ECSStorage*>(_storage)->template Get<T>(_entity);
        }
        
        template<typename T>
        void MarkComponentUpdated()
        {
            _storage->template MarkComponentUpdated<T>(_entity);
        }

    private:
        friend class ECSStorage<Components...>;

        EntityHandle(ECSStorage* storage, Entity entity)
            : _storage(storage), _entity(entity)
        {}

        ECSStorage* _storage = nullptr;
        Entity _entity = InvalidEntity;
    };

    ECSStorage()
    {
        _aliveList.push_back(false);
    }

    void Clear()
    {
        _aliveList.clear();
        _aliveList.push_back(false);
        _freeList.clear();
        ClearPools<0>();
    }

    Entity AllocateEntitySlot()
    {
        if (!_freeList.empty())
        {
            Entity reused = _freeList.back();
            _freeList.pop_back();
            _aliveList[reused] = true;
            return reused;
        }

        Entity created = static_cast<Entity>(_aliveList.size());
        _aliveList.push_back(true);
        return created;
    }

    EntityHandle CreateEntity()
    {
        return EntityHandle(this, AllocateEntitySlot());
    }

    EntityHandle GetEntityHandle(Entity entity)
    {
        return EntityHandle(this, entity);
    }

    void DestroyEntity(Entity entity)
    {
        if (!IsAlive(entity)) { return; }

        RemoveAllComponents<0>(entity);
        _aliveList[entity] = false;
        _freeList.push_back(entity);
    }

    bool IsAlive(Entity entity) const
    {
        return entity != InvalidEntity && entity < _aliveList.size() && _aliveList[entity];
    }

    size_t GetEntityCount() const
    {
        size_t count = 0;
        for (size_t i = 0; i < _aliveList.size(); ++i)
        {
            if (_aliveList[i]) { ++count; }
        }
        return count;
    }

    template<typename T, typename... Args>
    T& Add(Entity entity, Args&&... args)
    {
        static_assert(IsOneOf<T, Components...>::value, "T is not registered in ECSStorage");
        return GetPool<T>().Emplace(entity, std::forward<Args>(args)...);
    }

    template<typename T>
    void Remove(Entity entity)
    {
        static_assert(IsOneOf<T, Components...>::value, "T is not registered in ECSStorage");
        GetPool<T>().Remove(entity);
    }

    template<typename T>
    bool Has(Entity entity) const
    {
        static_assert(IsOneOf<T, Components...>::value, "T is not registered in ECSStorage");
        return GetPool<T>().Has(entity);
    }

    template<typename T>
    T& Get(Entity entity)
    {
        static_assert(IsOneOf<T, Components...>::value, "T is not registered in ECSStorage");
        return GetPool<T>().Get(entity);
    }

    template<typename T>
    const T& Get(Entity entity) const
    {
        static_assert(IsOneOf<T, Components...>::value, "T is not registered in ECSStorage");
        return GetPool<T>().Get(entity);
    }

    template<typename T>
    ComponentPool<T>& GetPool()
    {
        static_assert(IsOneOf<T, Components...>::value, "T is not registered in ECSStorage");
        return std::get<ComponentPool<T>>(_pools);
    }

    template<typename T>
    const ComponentPool<T>& GetPool() const
    {
        static_assert(IsOneOf<T, Components...>::value, "T is not registered in ECSStorage");
        return std::get<ComponentPool<T>>(_pools);
    }
    
    template<typename T, typename Func>
    void ForEach(Func&& func)
    {
        static_assert(IsOneOf<T, Components...>::value, "T is not registered in ECSStorage");

        ComponentPool<T>& pool = GetPool<T>();
        const std::vector<Entity>& entities = pool.GetEntities();
        std::vector<T>& data = pool.GetDense();

        for (size_t i = 0; i < data.size(); ++i)
        {
            func(entities[i], data[i]);
        }
    }

    template<typename First, typename Second, typename... Rest, typename Func>
    void ForEach(Func&& func)
    {
        static_assert(IsOneOf<First, Components...>::value, "First is not registered in ECSStorage");
        static_assert(IsOneOf<Second, Components...>::value, "Second is not registered in ECSStorage");
        static_assert((IsOneOf<Rest, Components...>::value && ...), "Rest are not registered in ECSStorage");

        ComponentPool<First>& firstPool = GetPool<First>();
        const std::vector<Entity>& entities = firstPool.GetEntities();
        std::vector<First>& firstData = firstPool.GetDense();

        for (size_t i = 0; i < firstData.size(); ++i)
        {
            Entity entity = entities[i];

            if (!Has<Second>(entity) || !(Has<Rest>(entity) && ...)) { continue; }

            func(entity, firstData[i], Get<Second>(entity), Get<Rest>(entity)...);
        }
    }
    
    // todo 
    // auto renderComponent = ecs.GetComponent<RenderComponent>(entity);
    // renderComponent.Material = newMaterial;
    // ecs.MarkCompomentUpdated<RenderComponent>(entity);
    
    template<typename T>
    void MarkComponentUpdated(Entity entity)
    {
        static_assert(IsOneOf<T, Components...>::value, "T is not registered in ECSStorage");
        GetPool<T>().MarkComponentUpdated(entity);
    }

private:
    template<size_t I>
    typename std::enable_if<I == sizeof...(Components), void>::type ClearPools()
    {
    }

    template<size_t I>
    typename std::enable_if < I < sizeof...(Components), void>::type ClearPools()
    {
        std::get<I>(_pools).Clear();
        ClearPools<I + 1>();
    }

    template<size_t I>
    typename std::enable_if<I == sizeof...(Components), void>::type RemoveAllComponents(Entity)
    {
    }

    template<size_t I>
    typename std::enable_if < I < sizeof...(Components), void>::type RemoveAllComponents(Entity entity)
    {
        std::get<I>(_pools).Remove(entity);
        RemoveAllComponents<I + 1>(entity);
    }

private:
    std::tuple<ComponentPool<Components>...> _pools;
    std::vector<uint8_t> _aliveList;
    std::vector<Entity> _freeList;
};
