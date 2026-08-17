#include "App.Base/Physics/JoltPhysicsBackend.h"

#include <algorithm>
#include <thread>

#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/PhysicsSystem.h>

namespace ObjectLayers
{
    static constexpr JPH::ObjectLayer Static = 0;
    static constexpr JPH::ObjectLayer Dynamic = 1;
    static constexpr JPH::ObjectLayer Count = 2;
}

namespace BroadPhaseLayers
{
    static constexpr JPH::BroadPhaseLayer Static(0);
    static constexpr JPH::BroadPhaseLayer Dynamic(1);
    static constexpr uint32_t Count = 2;
}

class ObjectLayerPairFilterImpl final : public JPH::ObjectLayerPairFilter
{
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override
    {
        switch (a)
        {
        case ObjectLayers::Static:
            return b == ObjectLayers::Dynamic;

        case ObjectLayers::Dynamic:
            return true;

        default:
            JPH_ASSERT(false);
            return false;
        }
    }
};

class BroadPhaseLayerInterfaceImpl final : public JPH::BroadPhaseLayerInterface
{
    public:
    BroadPhaseLayerInterfaceImpl()
    {
        _mapping[ObjectLayers::Static] = BroadPhaseLayers::Static;
        _mapping[ObjectLayers::Dynamic] = BroadPhaseLayers::Dynamic;
    }
    
    uint32_t GetNumBroadPhaseLayers() const override
    {
        return BroadPhaseLayers::Count;
    }
    
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override
    {
        JPH_ASSERT(layer < ObjectLayers::Count);
        return _mapping[layer];
    }
    
    
private:
    JPH::BroadPhaseLayer _mapping[ObjectLayers::Count];
};

class ObjectVsBroadPhaseLayerFilterImpl final : public JPH::ObjectVsBroadPhaseLayerFilter
{
public:
    bool ShouldCollide(JPH::ObjectLayer objectLayer, JPH::BroadPhaseLayer broadPhaseLayer) const override
    {
        switch (objectLayer)
        {
            case ObjectLayers::Static:
                return broadPhaseLayer == BroadPhaseLayers::Static;
            case ObjectLayers::Dynamic:
                return true;
            default:
                JPH_ASSERT(false);
                return false;
        }
    }
};
